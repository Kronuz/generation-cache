# generation-cache

[![CI](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml)


A small, header-only, thread-safe **cache invalidated as a whole group**, for
C++17 and later.

## What it is

Most caches evict by recency (LRU) or age (TTL) -- see
[`lru-cache`](https://github.com/Kronuz/lru-cache) for that shape. This one is
for a different, common case: many readers derive a `Value` from some `Key`
against a single shared, mutable source of truth. That source changes as a
discrete sequence of events (a write, a new version, an accepted update), not
continuously. Every cached `Value` is valid between two consecutive events and
invalid the instant the next one lands -- there is no "older but still useful"
state, so there is nothing for LRU or TTL to rank.

`generation_cache::Cache<Key, Value>` models exactly that: a `clear()` call
invalidates every entry at once and advances an internal generation counter.
A result computed from the OLD state, but not yet inserted when `clear()`
runs, is detected and silently dropped instead of resurrecting stale data --
this is the race the generation counter exists to close, not just a cache
API nicety.

## Install

Header-only: `generation_cache.hh` is the whole library. Requires C++17
(`std::optional`) and nothing else beyond the standard library.

With CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  generation_cache
  GIT_REPOSITORY https://github.com/Kronuz/generation-cache.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(generation_cache)

target_link_libraries(your_target PRIVATE generation_cache::generation_cache)
```

Then:

```cpp
#include "generation_cache.hh"
```

## Usage

The motivating case: a server composes an expensive `Value` from a `Key`
against its current state, and that state changes in discrete steps (e.g. one
per accepted write).

```cpp
#include "generation_cache.hh"

generation_cache::Cache<int, std::string> cache;

// A reader: look up, and on a miss, compute and insert.
auto lookup = cache.get_with_generation(key);
if (lookup.value) {
    return *lookup.value; // cache hit, no recompute
}
auto value = expensive_compute(key); // reads whatever the current state is
cache.try_insert(key, value, lookup.generation);
return value;

// The writer, whenever the source state changes:
cache.clear(); // every cached value was relative to the OLD state
```

`try_insert` only commits if `lookup.generation` still matches the cache's
current generation. If a `clear()` happened between the `get_with_generation`
call and the `try_insert` call -- e.g. because `expensive_compute` took a
while and a write landed in the meantime -- the insert is silently dropped:
the next reader recomputes against the new state instead of seeing a value
computed from the old one.

A reader with nothing to insert on a miss can use the simpler `get`:

```cpp
if (auto value = cache.get(key)) {
    return *value;
}
```

## API reference

```cpp
namespace generation_cache {

template <typename Key, typename Value>
class Cache {
public:
    using Generation = std::uint64_t;

    struct Lookup {
        std::optional<Value> value;
        Generation generation;
    };

    Generation generation() const;
    std::optional<Value> get(const Key& key) const;
    Lookup get_with_generation(const Key& key) const;
    bool try_insert(const Key& key, Value value, Generation at_generation);
    void clear();
    std::size_t size() const;
};

} // namespace generation_cache
```

- **`generation()`** -- the current generation, if you want to snapshot it
  separately from a lookup.
- **`get(key)`** -- a plain lookup; `std::nullopt` on a miss.
- **`get_with_generation(key)`** -- a lookup plus the generation it was read
  at, for a caller about to recompute on a miss.
- **`try_insert(key, value, at_generation)`** -- commits only if
  `at_generation` still matches the current generation; returns whether it
  committed. Never throws, never corrupts an already-current entry.
- **`clear()`** -- drops every entry and advances the generation.
- **`size()`** -- current entry count.

All methods are thread-safe (one internal mutex guards the map and the
generation counter together, so a lookup and a `clear()` can never interleave
inconsistently).

## What it isn't

- Not an LRU or TTL cache -- entries don't expire individually or by access
  recency. Use [`lru-cache`](https://github.com/Kronuz/lru-cache) for that.
- Not lock-free -- one mutex per `Cache` instance. For this library's
  motivating case (an occasional write, many reads composing a moderately
  expensive result), mutex contention on the cache itself is not the
  bottleneck; the work it's caching is.
- Not bounded by itself -- nothing here caps `size()`. In the motivating case,
  the number of distinct `Key`s ever reachable is naturally bounded by
  whatever upstream window the `Value`s are computed from (see
  [ARCHITECTURE.md](ARCHITECTURE.md)); a use case without such a natural bound
  needs its own cap.

## Build & test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The test covers the ordinary hit/miss path, `clear()` dropping every entry,
a stale `try_insert` (captured generation older than current) being silently
rejected, and a multi-threaded `try_insert`/`clear` race run under
TSan/ASan in CI to confirm no data race, not just no visible corruption.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour:

```sh
cmake -B build && cmake --build build && ./build/generation_cache_demo
```

It models three readers hitting a warm cache, a source-state change,
and the cache correctly recomputing once and then re-warming.

## Provenance

Extracted from [Detent](https://github.com/Kronuz/JustKnobs), where it
replaced an ad hoc composed-diff cache in the distributor's HTTP layer (many
pollers behind the same base version all paying to recompute the identical
diff). See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and
[AGENTS.md](AGENTS.md) for the repo map and invariants.

## License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
