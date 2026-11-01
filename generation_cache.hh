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
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include <utility>

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

	// Insert/replace/find/clear-only open-addressing table (linear probing,
	// power-of-two capacity, load <= 0.5). The cache never erases single
	// entries, so there are no tombstones. One slot holds the mixed hash (0 =
	// empty), so a probe compares the 64-bit hash before touching the key.
	template <typename Key, typename Value>
	class hash_entries {
		struct Slot {
			std::uint64_t h = 0;
			std::optional<std::pair<Key, Value>> kv;
		};
		std::vector<Slot> slots_;
		std::size_t size_ = 0;
		unsigned shift_ = 64;
		unsigned bits_ = 0;

		// Per-table seed folded into the hash before mixing, so the table index of a key is not
		// a fixed function of std::hash: the process seed varied by the table's address.
		std::uint64_t seed_ = splitmix64(process_seed() ^ reinterpret_cast<std::uintptr_t>(this));

		std::uint64_t mix(const Key& k) const {
			std::uint64_t h = static_cast<std::uint64_t>(std::hash<Key>{}(k)) ^ seed_;
			h *= 0x9E3779B97F4A7C15ULL;
			h ^= h >> 29;
			h *= 0xBF58476D1CE4E5B9ULL;
			return (h ^ (h >> 32)) | 1;  // never 0
		}
		std::size_t home(std::uint64_t h) const { return static_cast<std::size_t>(h >> shift_); }

		// Strong guarantee: the new table is built completely (in a local
		// vector, with the new shift) before it replaces the old one, and the
		// entries are moved only when that cannot throw, otherwise copied, so
		// a throw leaves the old table untouched.
		void grow() {
			const unsigned bits = bits_ ? bits_ + 1 : 4;  // 16 slots first
			const unsigned shift = 64 - bits;
			std::vector<Slot> next(std::size_t(1) << bits);
			const std::size_t mask = next.size() - 1;
			for (auto& s : slots_) {
				if (!s.h) continue;
				std::size_t i = static_cast<std::size_t>(s.h >> shift);
				while (next[i].h) i = (i + 1) & mask;
				if constexpr (std::is_nothrow_move_constructible_v<Key> && std::is_nothrow_move_constructible_v<Value>) {
					next[i].kv.emplace(std::move(s.kv->first), std::move(s.kv->second));
				} else if constexpr (std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Value>) {
					next[i].kv.emplace(s.kv->first, s.kv->second);
				} else {
					next[i].kv.emplace(std::move(s.kv->first), std::move(s.kv->second));
				}
				next[i].h = s.h;
			}
			slots_.swap(next);
			bits_ = bits;
			shift_ = shift;
		}
	public:
		const Value* find(const Key& key) const {
			if (!size_) return nullptr;
			const std::uint64_t h = mix(key);
			const std::size_t mask = slots_.size() - 1;
			for (std::size_t i = home(h);; i = (i + 1) & mask) {
				const Slot& s = slots_[i];
				if (!s.h) return nullptr;
				if (s.h == h && s.kv->first == key) return &s.kv->second;
			}
		}
		void insert_or_assign(const Key& key, Value&& value) {
			if ((size_ + 1) * 2 > slots_.size()) grow();
			const std::uint64_t h = mix(key);
			const std::size_t mask = slots_.size() - 1;
			for (std::size_t i = home(h);; i = (i + 1) & mask) {
				Slot& s = slots_[i];
				if (!s.h) {
					// Construct before publishing the slot: if the Key copy
					// (or Value move) throws, the slot stays empty (h == 0)
					// and size_ is untouched (strong guarantee).
					s.kv.emplace(key, std::move(value));
					s.h = h;
					++size_;
					return;
				}
				if (s.h == h && s.kv->first == key) {
					// Overwrite: strong only if Value's move assignment cannot
					// throw (a throwing one can leave the old value half-assigned).
					s.kv->second = std::move(value);
					return;
				}
			}
		}
		void clear() { slots_.clear(); size_ = 0; shift_ = 64; bits_ = 0; }
		std::size_t size() const { return size_; }
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
