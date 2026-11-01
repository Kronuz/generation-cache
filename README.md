# generation-cache

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/generation-cache/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only thread-safe cache for C++17 that clears as a whole and drops values computed before the last clear().
Many readers derive a `Value` from a `Key` against one source of truth that
changes in discrete steps, so every cached value turns wrong at the same moment.
`clear()` advances a generation counter, and `try_insert` refuses a value read
at an older generation, so a slow reader cannot put stale data back. Reach for
it when one write invalidates everything at once.

## ✨ Highlights

- **Whole-cache invalidation.** `clear()` drops every entry and advances the
  generation in one step.
- **No stale inserts.** `try_insert(key, value, generation)` commits only when
  the generation still matches, and returns whether it did.
- **Parallel readers.** One reader-writer lock: a reader enters with one atomic
  add, and a waiting writer holds new readers back, so neither side starves.
- **Strong exception guarantee.** If a `Key` copy, a `Value` move or copy, an
  allocation, or a rehash throws, `try_insert` leaves the cache unchanged
  (basic guarantee for move-only types whose move can throw).
- **A small open-addressing table.** Hashable keys live in a seeded
  linear-probing table that allocates 4 slots on the first insert; keys with
  only `operator<` use `std::map`.
- **Construction never throws**, even when the seed's entropy source does.

## 🚀 Quick start

```cpp
#include "generation_cache.hh"

#include <cstdio>
#include <string>

int main() {
	generation_cache::Cache<int, std::string> cache;
	int version = 1;  // the shared source of truth
	auto compute = [&](int key) { return std::to_string(key) + "@v" + std::to_string(version); };

	auto lookup = cache.get_with_generation(7);  // a miss, and the generation it saw
	cache.try_insert(7, compute(7), lookup.generation);  // commits: nothing changed meanwhile
	std::printf("%s\n", cache.get(7)->c_str());  // 7@v1

	auto slow = cache.get_with_generation(8);
	std::string stale = compute(8);  // computed against version 1...
	++version;
	cache.clear();  // ...but the source changed before the insert
	std::printf("%d\n", cache.try_insert(8, stale, slow.generation));  // 0: dropped
	std::printf("%zu\n", cache.size());  // 0
}
```

[`examples/demo.cc`](examples/demo.cc) runs three readers against a warm cache
through a source change.

## 📦 Installation

Requires a C++17 compiler and CMake 3.14 or newer; no dependencies. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan,
and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(generation_cache
  GIT_REPOSITORY https://github.com/Kronuz/generation-cache.git
  GIT_TAG        189d51b8251219e433aa11a5abe59a77a7efea88)  # or main for the tip
FetchContent_MakeAvailable(generation_cache)

target_link_libraries(my_app PRIVATE generation_cache::generation_cache)
```

```cpp
#include "generation_cache.hh"
```

## 🎯 When to use it

Use it for values derived from shared state that changes in discrete steps (a
write, a new version, an accepted update), read by many threads between steps.

Not for:

- **Recency or age eviction.** Entries never expire on their own;
  [lru-cache](https://github.com/Kronuz/lru-cache) evicts by recency.
- **Unbounded key spaces.** Nothing caps `size()`; a use without a natural
  bound on distinct keys needs its own cap.
- **Re-entrant callbacks.** A `Value` copy constructor, `std::hash`, and
  `operator==` run under the lock and must not call back into the same cache.
- **Crafted keys with unseeded hashes.** Keys with equal `std::hash` values
  collide in every cache, and the standard library's string hashes are
  unseeded; supply your own `std::hash` against adversarial keys.

## ⚡ Performance

ns per `get` that hits, lower is faster; every cell is the elapsed time divided by the number of `get` calls all threads complete. `Cache` is `generation_cache::Cache`. The other columns are hand-written caches of the same shape, each behind one lock: `std::map` + `std::mutex` is the usual hand-written cache, and `absl` + `std::mutex` and `absl` + `std::shared_mutex` are `absl::flat_hash_map` from [abseil](https://github.com/abseil/abseil-cpp) 20260817.0 behind a `std::mutex` and behind a `std::shared_mutex`. A row is a machine, the number of string keys of about 30 bytes and the number of reader threads, or the small cache of 64 `int` keys read by one thread. The bench first checks that every comparator returns the same result as `Cache` for every key it looks up, hits and misses, and aborts on a mismatch.

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---:|---:|---:|---:|
| Apple M4 Pro, 100000 keys, 1 thread | 682 | 142 | 122 | 175 |
| Apple M4 Pro, 100000 keys, 4 threads | 2297 | 72.9 | 578 | 284 |
| Apple M4 Pro, 64 `int` keys | 7.82 | 3.53 | 5.65 | 9.93 |
| Intel Core i9-9980HK, 100000 keys, 1 thread | 1049 | 434 | 396 | 447 |
| Intel Core i9-9980HK, 100000 keys, 4 threads | 2855 | 125 | 1857 | 599 |
| Intel Core i9-9980HK, 64 `int` keys | 21.1 | 10.9 | 17.4 | 37.5 |

On one thread `absl` behind a `std::mutex` is faster than `Cache` at 100000 keys, by 16% on the M4 Pro and 10% on the i9. With four threads `Cache` is 8 times faster than that comparator on the M4 Pro and 15 times on the i9, because readers share the lock, and it is the fastest on the small cache. Apple M4 Pro (macOS) and Intel Core i9-9980HK (macOS, x86-64), Apple clang 17, `-O3 -DNDEBUG`; abseil is the Homebrew `abseil` 20260817.0 on both, found by CMake and linked only by the bench. Each cell is the fastest of several runs; the runs, the other key counts and the method are in [docs/perf/](docs/perf/README.md). To rebuild and rerun:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGENERATION_CACHE_BUILD_BENCH=ON
cmake --build build --target generation_cache_bench
./build/generation_cache_bench          # the table
./build/generation_cache_bench check    # only the same-result check
```

## 📚 Documentation

- [docs/API.md](docs/API.md): the class, the reader pattern, thread and
  exception safety, and hashing.
- [ARCHITECTURE.md](ARCHITECTURE.md): the race `clear()` closes, the lock, and
  the container.
- [docs/perf/](docs/perf/README.md): the full benchmark tables.
- [CHANGELOG.md](CHANGELOG.md): what changed.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#generation-cache)
  in Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

The tests cover hits and misses, `clear()`, stale inserts, a threaded
`try_insert`/`clear` race (run under TSan and ASan in CI), the reader-writer
lock, a differential check against a reference cache, and injected throws at
every point of an insert sequence that crosses several rehashes. Add
`-DGENERATION_CACHE_BUILD_BENCH=ON` to build `generation_cache_bench`.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE). Extracted
from [Detent](https://github.com/Kronuz/Detent), where it caches composed diffs
so many pollers behind the same base version share one computed diff.
