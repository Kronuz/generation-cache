# generation-cache

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml)


A small, header-only, thread-safe **cache invalidated as a whole group**, for
C++17 and later.

## What it is

Most caches evict by recency (LRU) or age (TTL); see
[`lru-cache`](https://github.com/Kronuz/lru-cache) for that shape. This one is
for a different, common case: many readers derive a `Value` from some `Key`
against a single shared, mutable source of truth. That source changes as a
discrete sequence of events (a write, a new version, an accepted update), not
continuously. Every cached `Value` is valid between two consecutive events and
invalid the instant the next one lands. There is no "older but still useful"
state, so there is nothing for LRU or TTL to rank.

`generation_cache::Cache<Key, Value>` models exactly that: a `clear()` call
invalidates every entry at once and advances an internal generation counter.
A result computed from the OLD state, but not yet inserted when `clear()`
runs, is detected and silently dropped instead of resurrecting stale data.
Closing that race is what the generation counter is for.

## Install

Header-only: `generation_cache.hh` is the whole library. Requires C++17
(`std::optional`, `std::shared_mutex`) and nothing beyond the standard library.

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
call and the `try_insert` call (because `expensive_compute` took a while and
a write landed in the meantime, say), the insert is silently dropped:
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

- **`generation()`**: the current generation, if you want to snapshot it
  separately from a lookup.
- **`get(key)`**: a plain lookup; `std::nullopt` on a miss.
- **`get_with_generation(key)`**: a lookup plus the generation it was read
  at, for a caller about to recompute on a miss.
- **`try_insert(key, value, at_generation)`**: commits only if
  `at_generation` still matches the current generation; returns whether it
  committed. A stale call never throws and never overwrites a current entry.
- **`clear()`**: drops every entry and advances the generation.
- **`size()`**: current entry count.

All methods are thread-safe (one internal `std::shared_mutex` guards the table
and the generation counter together, so a lookup and a `clear()` can never
interleave inconsistently). `get`, `get_with_generation`, `generation` and
`size` take it shared, so readers run in parallel; `try_insert` and `clear`
take it exclusively. `std::shared_mutex` has no specified fairness policy
(glibc prefers readers), so readers that overlap continuously can delay
`try_insert`/`clear`. Each hold is a single probe, so readers overlap only
briefly; shard the cache if a workload saturates it with readers.
`try_insert` has the strong exception guarantee: if copying the key throws, the
cache is unchanged.

Entries have no exposed order. When `Key` has a `std::hash` and `operator==`,
they live in a small open-addressing table: linear probing, load <= 0.5, and the
64-bit hash stored per slot. The hash is mixed with a per-cache random seed, so
probe sequences are not predictable from `std::hash`. A `Key` with only
`operator<` uses `std::map`.

## Performance

Apple arm64, string keys, `get` hit, n = 100000, on a shared machine (absolute
numbers are noisy): 116 ns per op on one thread and 294 ns at 4 threads.
`std::shared_mutex` plus `absl::flat_hash_map` measures 99 and 284 ns on the
same run. The benchmark needs abseil installed:

```sh
cmake -B build && cmake --build build --target generation_cache_bench
./build/generation_cache_bench
```

## What it isn't

- Not an LRU or TTL cache. Entries don't expire individually or by access
  recency. Use [`lru-cache`](https://github.com/Kronuz/lru-cache) for that.
- Not lock-free. Each `Cache` instance has one reader/writer lock. For this library's
  motivating case (an occasional write, many reads composing a moderately
  expensive result), mutex contention on the cache itself is not the
  bottleneck; the work it's caching is.
- Not bounded by itself. Nothing here caps `size()`. In the motivating case,
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
rejected, and a multi-threaded `try_insert`/`clear` race that CI runs under
TSan and ASan to catch data races. `test/differential.cc` checks the cache
against a reference implementation.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour:

```sh
cmake -B build && cmake --build build && ./build/generation_cache_demo
```

It models three readers hitting a warm cache, a source-state change,
and the cache correctly recomputing once and then re-warming.

## Provenance

Extracted from [Detent](https://github.com/Kronuz/Detent), where it caches
composed diffs in the distributor's HTTP layer, so many pollers behind the same
base version share one computed diff. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the design, [AGENTS.md](AGENTS.md) for the repo map and invariants, and
[CHANGELOG.md](CHANGELOG.md) for what changed.

## License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
