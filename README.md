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

Header-only: `generation_cache.hh` is the whole library. Requires C++17 and
nothing beyond the standard library.

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

All methods are thread-safe. One internal reader-writer lock guards the table
and the generation counter together, so a lookup and a `clear()` can never
interleave inconsistently. `get`, `get_with_generation`, `generation` and `size`
take it shared, so readers run in parallel: a reader enters with one atomic add
and leaves with one subtract, so a lookup in a small cache costs about what the
lookup itself costs. `try_insert` and `clear` take it exclusively and wait only
for the readers already inside; a waiting writer holds new readers back, so
readers that overlap continuously cannot starve it, on every platform. A `Value`
copy constructor, `std::hash` and `operator==` run under the lock and must not
call back into the same cache. `try_insert` has the strong exception guarantee:
if anything throws (a `Key` copy, a `Value` move or copy, an allocation, the
table's rehash), the cache is unchanged and still usable. The rehash builds the
new table completely before it replaces the old one, and moves entries only when
`Key` and `Value` are nothrow-move-constructible, copying them otherwise. A
move-only type whose move can throw gets the basic guarantee instead, as with
`std::vector`, and overwriting an existing key assigns `Value`, which is strong
only if its move assignment cannot throw. Constructing a `Cache` never throws.

Entries have no exposed order. When `Key` has a `std::hash` and `operator==`,
they live in a small open-addressing table: linear probing, load <= 0.5, the
64-bit hash stored per slot, and 4 slots allocated by the first insert. The hash is mixed with a seed drawn once per process
(from `std::random_device`, mixed with the clock, the thread id and an address,
so an entropy source that fails costs quality and never an exception) and varied
per table, so the index of a key is not a function of `std::hash` alone. The seed
spreads the indexes of keys whose `std::hash` values differ. Keys with equal
`std::hash` values collide in every cache, and the standard library's string
hashes are unseeded, so a caller that must withstand crafted keys needs its own
`std::hash`. A `Key` with only `operator<` uses `std::map`.

## Performance

String keys of about 30 bytes, `get` hit, ns per op (best of 5, mean of two passes), on
1 thread and on 4 threads; the comparators are `std::map` behind a `std::mutex` and
`absl::flat_hash_map` behind a `std::mutex` or a `std::shared_mutex`. A small cache
holds 64 `int` keys on 1 thread. Apple M4 Pro (arm64, Apple clang 17):

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 100000 keys, 1 thread | 723 | 147 | 135 | 194 |
| 100000 keys, 4 threads | 2569 | 84.4 | 622 | 293 |
| 64 `int` keys: construct + destroy | 12.5 | 5.7 | 11.5 | 22.4 |
| 64 `int` keys: get hit | 7.9 | 3.5 | 5.4 | 10.2 |
| 64 `int` keys: get miss | 8.6 | 3.5 | 5.1 | 10.1 |

Intel i9-9980HK (x86_64, Apple clang 17):

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 100000 keys, 1 thread | 1098 | 440 | 410 | 445 |
| 100000 keys, 4 threads | 2713 | 134 | 1789 | 574 |
| 64 `int` keys: construct + destroy | 62.6 | 24.5 | 50.1 | 81.1 |
| 64 `int` keys: get hit | 19.1 | 10.6 | 17.1 | 36.7 |
| 64 `int` keys: get miss | 20.9 | 10.7 | 17.3 | 36.4 |

The benchmark needs abseil installed. Arguments select the rows: sizes run only those
string rows, and `small` adds the small-cache rows (no arguments runs 64, 4096 and
100000, then the small-cache rows):

```sh
cmake -B build && cmake --build build --target generation_cache_bench
./build/generation_cache_bench [n ...] [small]
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
against a reference implementation. `test/exception_safety.cc` injects throwing
copies and moves and `std::bad_alloc` at every point of an insert sequence that
crosses several rehashes, and checks the cache against a `std::map` model after
each failure.

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
