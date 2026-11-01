/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

// A thread-safe cache whose entries are invalidated as a WHOLE GROUP by a
// generation bump, not individually by LRU/TTL (see lru-cache for that
// shape instead). Built for the case where many readers derive a Value
// from some Key against a shared, mutable source of truth, that source
// changes as a discrete sequence of events (not continuously), and EVERY
// cached Value is valid between two consecutive events and invalid after
// the next one -- a per-request HTTP response cache keyed by "what the
// client already has", invalidated whenever the server accepts a new
// write, is the motivating case.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <utility>

// The table probes a group of control bytes at a time: 16 with SSE2 or NEON, 8 in a 64-bit word
// otherwise. GENERATION_CACHE_NO_SIMD selects the 64-bit word everywhere.
#if !defined(GENERATION_CACHE_NO_SIMD) && defined(__SSE2__)
#include <emmintrin.h>
#define GENERATION_CACHE_GROUP_SSE2 1
#elif !defined(GENERATION_CACHE_NO_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define GENERATION_CACHE_GROUP_NEON 1
#endif

// The entropy source for the per-process hash seed: a class with a default constructor and an
// `unsigned operator()()`. A build can name another type, which is how the tests make it throw.
#ifndef GENERATION_CACHE_ENTROPY
#define GENERATION_CACHE_ENTROPY std::random_device
#endif

namespace generation_cache {

namespace detail {
	inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
		x += 0x9E3779B97F4A7C15ULL;
		x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
		x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
		return x ^ (x >> 31);
	}

	// The high and low halves of the 128-bit product, xored: every input bit reaches every output
	// bit through one multiplication. Without a 128-bit type it is splitmix64 of `a`.
	inline std::uint64_t fold_multiply(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
		const __uint128_t product = static_cast<__uint128_t>(a) * b;
		return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);
#else
		return splitmix64(a ^ (b >> 1));
#endif
	}

	// The seed every table of the process starts from, drawn once on first use. It mixes the
	// operating system's entropy with the clock, the thread and an address, so a failing
	// entropy source costs quality, never an exception: constructing a Cache cannot throw.
	inline std::uint64_t process_seed() noexcept {
		static const std::uint64_t seed = []() noexcept {
			std::uint64_t os = 0;
			try {
				GENERATION_CACHE_ENTROPY device;
				os = (std::uint64_t(device()) << 32) ^ device();
			} catch (...) {
			}
			static const char anchor = 0;
			const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			const auto tid = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			return splitmix64(os ^ splitmix64(now) ^ splitmix64(tid ^ reinterpret_cast<std::uintptr_t>(&anchor)));
		}();
		return seed;
	}

	// A reader-writer lock for short critical sections. A reader enters with one atomic add and
	// leaves with one subtract. A writer takes the mutex, sets the writer flag, which sends new
	// readers to wait on that mutex, and waits for the readers inside to leave. A waiting writer
	// therefore holds new readers back on every platform, and writers cannot be starved.
	class rw_lock {
		static constexpr std::uint32_t writer = 0x80000000u;
		std::atomic<std::uint32_t> state_{0};  // readers inside, plus `writer` while a writer holds or waits
		std::mutex mutex_;                     // held by a writer for its whole critical section
	public:
		void lock_shared() {
			if (!(state_.fetch_add(1, std::memory_order_acquire) & writer)) return;
			state_.fetch_sub(1, std::memory_order_relaxed);
			// A writer has the mutex until it clears the flag, and no writer can set the flag
			// without the mutex, so with the mutex held this add cannot meet the flag.
			std::lock_guard<std::mutex> turn(mutex_);
			state_.fetch_add(1, std::memory_order_acquire);
		}
		void unlock_shared() noexcept { state_.fetch_sub(1, std::memory_order_release); }
		void lock() {
			mutex_.lock();
			state_.fetch_or(writer, std::memory_order_acquire);
			for (unsigned spins = 0; (state_.load(std::memory_order_acquire) & ~writer) != 0; ++spins) {
				if (spins >= 64) std::this_thread::yield();
			}
		}
		void unlock() noexcept {
			state_.fetch_and(~writer, std::memory_order_release);
			mutex_.unlock();
		}
	};

	// A Key is "hashable" when std::hash<Key> is enabled and operator== exists.
	template <typename Key, typename = void>
	struct is_hashable : std::false_type {};
	template <typename Key>
	struct is_hashable<Key, std::void_t<
		decltype(std::declval<const std::hash<Key>&>()(std::declval<const Key&>())),
		decltype(static_cast<bool>(std::declval<const Key&>() == std::declval<const Key&>()))
	>> : std::bool_constant<std::is_default_constructible_v<std::hash<Key>>> {};

	inline unsigned count_trailing_zeros(std::uint64_t x) noexcept {  // x != 0
#if defined(__GNUC__) || defined(__clang__)
		return static_cast<unsigned>(__builtin_ctzll(x));
#else
		unsigned n = 0;
		while (!(x & 1)) { x >>= 1; ++n; }
		return n;
#endif
	}

	// The slots a group test selects: `Shift` bits per slot in `bits`, the lowest of them set.
	template <typename Raw, unsigned Shift>
	class slot_mask {
		Raw bits_;
	public:
		explicit slot_mask(Raw bits) noexcept : bits_(bits) {}
		explicit operator bool() const noexcept { return bits_ != 0; }
		unsigned first() const noexcept { return count_trailing_zeros(bits_) >> Shift; }  // the lowest slot selected
		void clear_first() noexcept { bits_ &= bits_ - 1; }
	};

	// A control byte is `empty`, or the 7-bit tag of the entry in its slot (0 to 0x7F), or `padding`
	// past the last slot of a table smaller than a group. A group test reads `width` consecutive
	// control bytes and selects the slots whose byte equals a tag, or is `empty`; `padding` is
	// neither.
	inline constexpr std::uint8_t ctrl_empty = 0x80;
	inline constexpr std::uint8_t ctrl_padding = 0xFF;

#if defined(GENERATION_CACHE_GROUP_SSE2)
	struct group {
		static constexpr std::size_t width = 16;
		using mask = slot_mask<std::uint32_t, 0>;
		__m128i bytes;
		explicit group(const std::uint8_t* p) noexcept : bytes(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))) {}
		mask equal(std::uint8_t value) const noexcept {
			return mask(static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(bytes, _mm_set1_epi8(static_cast<char>(value))))));
		}
	};
#elif defined(GENERATION_CACHE_GROUP_NEON)
	struct group {
		static constexpr std::size_t width = 16;
		using mask = slot_mask<std::uint64_t, 2>;
		uint8x16_t bytes;
		explicit group(const std::uint8_t* p) noexcept : bytes(vld1q_u8(p)) {}
		mask equal(std::uint8_t value) const noexcept {
			// Narrowing shift: 4 bits per slot, the high bit of each set for a match.
			const uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(bytes, vdupq_n_u8(value))), 4);
			return mask(vget_lane_u64(vreinterpret_u64_u8(narrowed), 0) & 0x8888888888888888ULL);
		}
	};
#else
	struct group {
		static constexpr std::size_t width = 8;
		using mask = slot_mask<std::uint64_t, 3>;
		std::uint64_t bytes;
		explicit group(const std::uint8_t* p) noexcept {
			std::memcpy(&bytes, p, sizeof bytes);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
			bytes = __builtin_bswap64(bytes);
#endif
		}
		// Exactly the bytes equal to `value`, in the high bit of each: no carry crosses a byte.
		mask equal(std::uint8_t value) const noexcept {
			constexpr std::uint64_t low7 = 0x7F7F7F7F7F7F7F7FULL;
			const std::uint64_t x = bytes ^ (0x0101010101010101ULL * value);
			return mask(~(((x & low7) + low7) | x | low7));
		}
	};
#endif

	// Insert/replace/find/clear-only open-addressing table. The cache never erases single
	// entries, so a slot goes from empty to full and never back, and there are no tombstones.
	//
	// Layout: one allocation of `capacity` control bytes (at least one group's worth), then
	// `capacity` slots. A slot holds a Key and a Value, with no hash and no flag of its own. The
	// capacity is a power of two, and the slots form groups of `group::width` (a table smaller than
	// a group is one group, padded). A key's hash picks its first group (low bits) and its tag (top
	// 7 bits); a lookup tests a whole group's control bytes at once, compares the keys of the slots
	// whose tag matches, and stops at the first group with an empty slot, because an entry is
	// always placed in the first group of its probe sequence that has room and groups never
	// empty. The probe sequence is triangular over groups, so it visits every group.
	// At most `capacity - max(1, capacity / 8)` slots are used, so a group with an empty slot
	// always exists and a probe terminates.
	template <typename Key, typename Value>
	class hash_entries {
		using Entry = std::pair<Key, Value>;
		static constexpr std::size_t width = group::width;

		// The first insert allocates this many slots, no more: a cache that holds a few entries
		// pays for a few, and a larger one grows by doubling.
		static constexpr std::size_t initial_capacity = 4;

		// A per-table seed folded into the hash, so the group of a key is not a fixed function of
		// std::hash: the process seed varied by the table's address.
		std::uint64_t seed_ = splitmix64(process_seed() ^ reinterpret_cast<std::uintptr_t>(this));
		std::uint8_t* ctrl_ = nullptr;  // `capacity_` control bytes (at least `width`), then the slots
		Entry* slots_ = nullptr;
		std::size_t group_mask_ = 0;    // the number of groups, less one
		std::size_t capacity_ = 0;      // slots; 0 before the first insert
		std::size_t size_ = 0;
		std::size_t room_ = 0;          // entries that fit before the table must grow

		hash_entries(const hash_entries&) = delete;
		hash_entries& operator=(const hash_entries&) = delete;

		static constexpr std::size_t max_size(std::size_t capacity) noexcept {
			return capacity - (capacity / 8 ? capacity / 8 : 1);
		}
		static constexpr std::size_t group_mask_of(std::size_t capacity) noexcept {
			return (capacity < width ? 1 : capacity / width) - 1;
		}
		static constexpr std::size_t ctrl_bytes(std::size_t capacity) noexcept {
			return capacity < width ? width : capacity;
		}
		// The slots start after the control bytes, at the Entry's alignment.
		static constexpr std::size_t slots_offset(std::size_t capacity) noexcept {
			return (ctrl_bytes(capacity) + alignof(Entry) - 1) / alignof(Entry) * alignof(Entry);
		}
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
		static constexpr bool over_aligned = alignof(Entry) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
		static constexpr bool over_aligned = alignof(Entry) > 16;
#endif
		static std::uint8_t* allocate_block(std::size_t capacity) {
			if (capacity > (static_cast<std::size_t>(-1) / 2 - width - alignof(Entry)) / sizeof(Entry)) throw std::bad_alloc();
			const std::size_t bytes = slots_offset(capacity) + capacity * sizeof(Entry);
			if constexpr (over_aligned) return static_cast<std::uint8_t*>(::operator new(bytes, std::align_val_t(alignof(Entry))));
			else return static_cast<std::uint8_t*>(::operator new(bytes));
		}
		static void free_block(std::uint8_t* block) noexcept {
			if constexpr (over_aligned) ::operator delete(block, std::align_val_t(alignof(Entry)));
			else ::operator delete(block);
		}
		static Entry* slots_of(std::uint8_t* block, std::size_t capacity) noexcept {
			return reinterpret_cast<Entry*>(block + slots_offset(capacity));
		}
		static void destroy_entries(const std::uint8_t* ctrl, Entry* slots, std::size_t capacity) noexcept {
			if constexpr (!std::is_trivially_destructible_v<Entry>) {
				for (std::size_t i = 0; i < capacity; ++i) {
					if (!(ctrl[i] & ctrl_empty)) slots[i].~Entry();
				}
			}
		}

		// std::hash must not throw (a hash requirement), so a rehash calls it freely.
		std::uint64_t mix(const Key& k) const {
			return fold_multiply(static_cast<std::uint64_t>(std::hash<Key>{}(k)) ^ seed_, 0x9E3779B97F4A7C15ULL);
		}
		static constexpr std::uint8_t tag_of(std::uint64_t h) noexcept { return static_cast<std::uint8_t>(h >> 57); }

		// The first slot with room on the probe path of `h` in a table of these control bytes.
		static std::size_t empty_slot(const std::uint8_t* ctrl, std::size_t group_mask, std::uint64_t h) noexcept {
			std::size_t g = static_cast<std::size_t>(h) & group_mask;
			for (std::size_t step = 1;; ++step) {
				if (const auto room = group(ctrl + g * width).equal(ctrl_empty)) return g * width + room.first();
				g = (g + step) & group_mask;
			}
		}

		// Strong guarantee: the new table is built completely, beside the old one, before it
		// replaces it, and the entries are moved only when that cannot throw, otherwise copied, so
		// a throw leaves the old table untouched.
		void grow() {
			const std::size_t capacity = capacity_ ? capacity_ * 2 : initial_capacity;
			const std::size_t group_mask = group_mask_of(capacity);
			std::uint8_t* const ctrl = allocate_block(capacity);
			Entry* const slots = slots_of(ctrl, capacity);
			std::memset(ctrl, ctrl_empty, capacity);
			if (capacity < width) std::memset(ctrl + capacity, ctrl_padding, width - capacity);
			try {
				for (std::size_t i = 0; i < capacity_; ++i) {
					if (ctrl_[i] & ctrl_empty) continue;
					Entry& from = slots_[i];
					const std::uint64_t h = mix(from.first);
					const std::size_t to = empty_slot(ctrl, group_mask, h);
					assert(to < capacity);
					if constexpr (std::is_nothrow_move_constructible_v<Key> && std::is_nothrow_move_constructible_v<Value>) {
						::new (static_cast<void*>(slots + to)) Entry(std::move(from.first), std::move(from.second));
					} else if constexpr (std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Value>) {
						::new (static_cast<void*>(slots + to)) Entry(from.first, from.second);
					} else {
						::new (static_cast<void*>(slots + to)) Entry(std::move(from.first), std::move(from.second));
					}
					ctrl[to] = tag_of(h);
				}
			} catch (...) {
				destroy_entries(ctrl, slots, capacity);
				free_block(ctrl);
				throw;
			}
			release();
			ctrl_ = ctrl;
			slots_ = slots;
			group_mask_ = group_mask;
			capacity_ = capacity;
			room_ = max_size(capacity) - size_;
		}

		void release() noexcept {
			if (!capacity_) return;
			destroy_entries(ctrl_, slots_, capacity_);
			free_block(ctrl_);
			ctrl_ = nullptr;
			slots_ = nullptr;
			group_mask_ = 0;
			capacity_ = 0;
		}

		// Construct the entry before publishing its control byte and the size: if the Key copy
		// (or the Value move) throws, the slot stays empty and the table is as it was.
		void place(std::size_t i, std::uint8_t tag, const Key& key, Value&& value) {
			assert(i < capacity_ && ctrl_[i] == ctrl_empty && room_ > 0);
			::new (static_cast<void*>(slots_ + i)) Entry(key, std::move(value));
			ctrl_[i] = tag;
			++size_;
			--room_;
		}
	public:
		hash_entries() = default;
		~hash_entries() { release(); }

		const Value* find(const Key& key) const {
			if (!size_) return nullptr;
			const std::uint64_t h = mix(key);
			const std::uint8_t tag = tag_of(h);
			std::size_t g = static_cast<std::size_t>(h) & group_mask_;
			for (std::size_t step = 1;; ++step) {
				const group probe(ctrl_ + g * width);
				for (auto match = probe.equal(tag); match; match.clear_first()) {
					assert(g * width + match.first() < capacity_);
					const Entry& e = slots_[g * width + match.first()];
					if (e.first == key) return &e.second;
				}
				if (probe.equal(ctrl_empty)) return nullptr;
				g = (g + step) & group_mask_;
			}
		}
		void insert_or_assign(const Key& key, Value&& value) {
			const std::uint64_t h = mix(key);
			const std::uint8_t tag = tag_of(h);
			if (capacity_) {
				std::size_t g = static_cast<std::size_t>(h) & group_mask_;
				for (std::size_t step = 1;; ++step) {
					const group probe(ctrl_ + g * width);
					for (auto match = probe.equal(tag); match; match.clear_first()) {
						Entry& e = slots_[g * width + match.first()];
						if (e.first == key) {
							// Overwrite: strong only if Value's move assignment cannot
							// throw (a throwing one can leave the old value half-assigned).
							e.second = std::move(value);
							return;
						}
					}
					if (const auto room = probe.equal(ctrl_empty)) {
						if (room_) {
							place(g * width + room.first(), tag, key, std::move(value));
							return;
						}
						break;
					}
					g = (g + step) & group_mask_;
				}
			}
			grow();
			place(empty_slot(ctrl_, group_mask_, h), tag, key, std::move(value));
		}
		void clear() noexcept {
			release();
			size_ = 0;
			room_ = 0;
		}
		std::size_t size() const { return size_; }
		std::size_t capacity() const { return capacity_; }

		// The groups a lookup of `key` examines (0 on an empty table, 1 when its first group
		// settles it): a diagnostic for the tests of the hash mixing and the probe sequence.
		std::size_t probes(const Key& key) const {
			if (!size_) return 0;
			const std::uint64_t h = mix(key);
			const std::uint8_t tag = tag_of(h);
			std::size_t g = static_cast<std::size_t>(h) & group_mask_;
			for (std::size_t step = 1;; ++step) {
				const group probe(ctrl_ + g * width);
				for (auto match = probe.equal(tag); match; match.clear_first()) {
					if (slots_[g * width + match.first()].first == key) return step;
				}
				if (probe.equal(ctrl_empty)) return step;
				g = (g + step) & group_mask_;
			}
		}
	};

	// std::map adapter exposing the same four operations.
	template <typename Key, typename Value>
	class ordered_entries {
		std::map<Key, Value> m_;
	public:
		const Value* find(const Key& key) const {
			auto it = m_.find(key);
			return it == m_.end() ? nullptr : &it->second;
		}
		// insert_or_assign constructs a new node from the value, so a throw
		// leaves the map unchanged; m_[key] = ... would first insert a
		// default-constructed Value and keep it if the assignment threw.
		void insert_or_assign(const Key& key, Value&& value) { m_.insert_or_assign(key, std::move(value)); }
		void clear() { m_.clear(); }
		std::size_t size() const { return m_.size(); }
	};

	// A hash table when Key has std::hash and ==; otherwise a std::map, so a
	// Key with only operator< still compiles.
	template <typename Key, typename Value, bool = is_hashable<Key>::value>
	struct entries_for { using type = hash_entries<Key, Value>; };
	template <typename Key, typename Value>
	struct entries_for<Key, Value, false> { using type = ordered_entries<Key, Value>; };
}

template <typename Key, typename Value>
class Cache {
public:
	using Generation = std::uint64_t;

	// A lookup result: the value (if present) and the generation it was
	// read at. A caller that recomputes on a miss carries `generation`
	// forward to try_insert() so the insert can detect a clear() that
	// happened while it was computing.
	struct Lookup {
		std::optional<Value> value;
		Generation generation = 0;
	};

	Cache() = default;
	Cache(const Cache&) = delete;
	Cache& operator=(const Cache&) = delete;

	// The current generation, for a caller that wants to snapshot it
	// separately from a lookup (e.g. before deciding a key isn't even
	// worth looking up).
	[[nodiscard]] Generation generation() const {
		std::shared_lock<detail::rw_lock> lock(mutex_);
		return generation_;
	}

	// A plain lookup, no generation returned -- use this when the caller
	// has nothing to insert on a miss (a read-only consumer).
	[[nodiscard]] std::optional<Value> get(const Key& key) const {
		std::shared_lock<detail::rw_lock> lock(mutex_);
		const Value* v = entries_.find(key);
		if (!v) return std::nullopt;
		return *v;
	}

	// Lookup plus the generation it was read at -- see Lookup's comment.
	// Use this when a miss means "I'm about to compute this myself and
	// will want to try_insert() the result".
	[[nodiscard]] Lookup get_with_generation(const Key& key) const {
		std::shared_lock<detail::rw_lock> lock(mutex_);
		Lookup result;
		result.generation = generation_;
		if (const Value* v = entries_.find(key)) result.value = *v;
		return result;
	}

	// Commits (key, value) only if `at_generation` still matches the
	// CURRENT generation -- i.e. no clear() happened between whatever
	// get_with_generation() call produced `at_generation` and this call.
	// Returns true if committed, false if silently discarded as stale.
	// Safe to call with a stale generation: never throws, never corrupts
	// an already-current entry with an outdated one.
	bool try_insert(const Key& key, Value value, Generation at_generation) {
		std::unique_lock<detail::rw_lock> lock(mutex_);
		if (generation_ != at_generation) return false;
		entries_.insert_or_assign(key, std::move(value));
		return true;
	}

	// Invalidates every entry at once and advances the generation, so any
	// try_insert() still carrying the OLD generation (its compute started
	// before this clear(), finished after) silently no-ops instead of
	// resurrecting a value that's no longer valid against whatever changed.
	void clear() {
		std::unique_lock<detail::rw_lock> lock(mutex_);
		entries_.clear();
		++generation_;
	}

	[[nodiscard]] std::size_t size() const {
		std::shared_lock<detail::rw_lock> lock(mutex_);
		return entries_.size();
	}

private:
	// Readers share the lock; try_insert and clear are exclusive, and a waiting writer holds
	// new readers back; see the README.
	mutable detail::rw_lock mutex_;
	typename detail::entries_for<Key, Value>::type entries_;
	Generation generation_ = 0;
};

} // namespace generation_cache
