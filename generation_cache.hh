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
#include <utility>

namespace generation_cache {

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
		std::lock_guard<std::mutex> lock(mutex_);
		return generation_;
	}

	// A plain lookup, no generation returned -- use this when the caller
	// has nothing to insert on a miss (a read-only consumer).
	[[nodiscard]] std::optional<Value> get(const Key& key) const {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = entries_.find(key);
		if (it == entries_.end()) return std::nullopt;
		return it->second;
	}

	// Lookup plus the generation it was read at -- see Lookup's comment.
	// Use this when a miss means "I'm about to compute this myself and
	// will want to try_insert() the result".
	[[nodiscard]] Lookup get_with_generation(const Key& key) const {
		std::lock_guard<std::mutex> lock(mutex_);
		Lookup result;
		result.generation = generation_;
		auto it = entries_.find(key);
		if (it != entries_.end()) result.value = it->second;
		return result;
	}

	// Commits (key, value) only if `at_generation` still matches the
	// CURRENT generation -- i.e. no clear() happened between whatever
	// get_with_generation() call produced `at_generation` and this call.
	// Returns true if committed, false if silently discarded as stale.
	// Safe to call with a stale generation: never throws, never corrupts
	// an already-current entry with an outdated one.
	bool try_insert(const Key& key, Value value, Generation at_generation) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (generation_ != at_generation) return false;
		entries_[key] = std::move(value);
		return true;
	}

	// Invalidates every entry at once and advances the generation, so any
	// try_insert() still carrying the OLD generation (its compute started
	// before this clear(), finished after) silently no-ops instead of
	// resurrecting a value that's no longer valid against whatever changed.
	void clear() {
		std::lock_guard<std::mutex> lock(mutex_);
		entries_.clear();
		++generation_;
	}

	[[nodiscard]] std::size_t size() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return entries_.size();
	}

private:
	mutable std::mutex mutex_;
	std::map<Key, Value> entries_;
	Generation generation_ = 0;
};

} // namespace generation_cache
