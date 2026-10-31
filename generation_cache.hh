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

#include <map>
#include <mutex>
#include <cstdint>
#include <optional>
#include <random>
#include <shared_mutex>
#include <type_traits>
#include <vector>
#include <utility>

namespace generation_cache {

namespace detail {
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

		// Per-table random seed folded into the hash before mixing, so the
		// probe sequence is not a fixed function of std::hash and a key set
		// cannot be crafted offline to collide in every cache.
		std::uint64_t seed_ = (std::uint64_t(std::random_device{}()) << 32) ^ std::random_device{}();

		std::uint64_t mix(const Key& k) const {
			std::uint64_t h = static_cast<std::uint64_t>(std::hash<Key>{}(k)) ^ seed_;
			h *= 0x9E3779B97F4A7C15ULL;
			h ^= h >> 29;
			h *= 0xBF58476D1CE4E5B9ULL;
			return (h ^ (h >> 32)) | 1;  // never 0
		}
		std::size_t home(std::uint64_t h) const { return static_cast<std::size_t>(h >> shift_); }

		void grow() {
			bits_ = bits_ ? bits_ + 1 : 4;  // 16 slots first
			std::vector<Slot> old(std::size_t(1) << bits_);
			old.swap(slots_);
			shift_ = 64 - bits_;
			for (auto& s : old) {
				if (!s.h) continue;
				std::size_t i = home(s.h);
				while (slots_[i].h) i = (i + 1) & (slots_.size() - 1);
				slots_[i] = std::move(s);
			}
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
		void insert_or_assign(const Key& key, Value&& value) { m_[key] = std::move(value); }
		void clear() { m_.clear(); }
		std::size_t size() const { return m_.size(); }
	};

	// Entries have no exposed order, so the container is an implementation
	// choice: a hash table (one probe instead of a tree walk of string
	// compares) when the key allows it, the original ordered map otherwise
	// (a Key that only has operator< still compiles).
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
		std::shared_lock<std::shared_mutex> lock(mutex_);
		return generation_;
	}

	// A plain lookup, no generation returned -- use this when the caller
	// has nothing to insert on a miss (a read-only consumer).
	[[nodiscard]] std::optional<Value> get(const Key& key) const {
		std::shared_lock<std::shared_mutex> lock(mutex_);
		const Value* v = entries_.find(key);
		if (!v) return std::nullopt;
		return *v;
	}

	// Lookup plus the generation it was read at -- see Lookup's comment.
	// Use this when a miss means "I'm about to compute this myself and
	// will want to try_insert() the result".
	[[nodiscard]] Lookup get_with_generation(const Key& key) const {
		std::shared_lock<std::shared_mutex> lock(mutex_);
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
		std::unique_lock<std::shared_mutex> lock(mutex_);
		if (generation_ != at_generation) return false;
		entries_.insert_or_assign(key, std::move(value));
		return true;
	}

	// Invalidates every entry at once and advances the generation, so any
	// try_insert() still carrying the OLD generation (its compute started
	// before this clear(), finished after) silently no-ops instead of
	// resurrecting a value that's no longer valid against whatever changed.
	void clear() {
		std::unique_lock<std::shared_mutex> lock(mutex_);
		entries_.clear();
		++generation_;
	}

	[[nodiscard]] std::size_t size() const {
		std::shared_lock<std::shared_mutex> lock(mutex_);
		return entries_.size();
	}

private:
	// Readers (get, get_with_generation, generation, size) share the lock;
	// try_insert and clear are exclusive. Fairness: std::shared_mutex does not
	// specify a policy and glibc's default prefers readers, so a continuous
	// stream of overlapping readers can delay try_insert/clear indefinitely in
	// theory. Lock hold times here are a single probe, so readers overlap only
	// briefly in practice; if a workload saturates the readers, shard the cache
	// or swap in a writer-preferring lock.
	mutable std::shared_mutex mutex_;
	typename detail::entries_for<Key, Value>::type entries_;
	Generation generation_ = 0;
};

} // namespace generation_cache
