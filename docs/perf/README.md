# generation-cache performance

`bench/bench.cc` times `get` on a warm cache, in ns per call, lower is faster.

- `Cache` is `generation_cache::Cache`.
- `std::map` + `std::mutex` is a hand-written cache: a `std::map` behind one `std::mutex`, the usual shape.
- `absl` + `std::mutex` and `absl` + `std::shared_mutex` are `absl::flat_hash_map` from
  [abseil](https://github.com/abseil/abseil-cpp) 20260817.0 behind a `std::mutex` and behind a
  `std::shared_mutex`. The Homebrew formula `abseil` 20260817.0 is installed on both machines, found by
  `find_package(absl)` and linked only by the bench target.
- A string row is `n` keys of about 30 bytes read by 1 or 4 threads; a cell is the elapsed time divided
  by the number of `get` calls all threads complete. The small-cache rows hold 64 `int` keys and one
  thread: `construct + destroy` of an empty cache, and `get` hit (64 keys present) and miss (64 keys absent).

The bench first checks that every comparator returns the same result as `Cache` (hit or miss,
and the same value on a hit) for every key it looks up, and aborts on a mismatch.
`generation_cache_bench check` runs only that check; CTest runs it as `generation_cache_bench_check`.

## Method

Both machines build with Apple clang 17, `-O3 -DNDEBUG` (a Release build, C++17). Within a run each
string cell is the best of 5 repetitions of 500000 `get` calls on 1 thread or 300000 per thread on 4, and
each small-cache cell the best of 7. A table cell is the fastest of the runs listed below. Both machines
are shared, and rows that read 100000 keys move with whatever else runs: the 1-thread, 100000-key cells of the M4 Pro differ by up to 113% between runs, the other rows by
at most 31%. The 10 to 16% lead of `absl` + `std::mutex` over `Cache` on one thread at 100000 keys is
inside the spread of the M4 Pro row; it holds in 7 of the 8 runs there and in all 3 runs on the i9.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGENERATION_CACHE_BUILD_BENCH=ON
cmake --build build --target generation_cache_bench
./build/generation_cache_bench [n ...] [small] [check]
```

Arguments select the rows: sizes run only those string rows, and `small` adds the small-cache rows. With no
arguments it runs 64, 4096 and 100000, then the small-cache rows.

## Apple M4 Pro, macOS (arm64), fastest of 8 runs

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 64 keys, 1 thread | 84.2 | 29.6 | 27.8 | 32.2 |
| 64 keys, 4 threads | 258 | 63.6 | 92.4 | 108 |
| 4096 keys, 1 thread | 233 | 47.7 | 43.6 | 50.5 |
| 4096 keys, 4 threads | 890 | 67.6 | 152 | 185 |
| 100000 keys, 1 thread | 682 | 142 | 122 | 175 |
| 100000 keys, 4 threads | 2297 | 72.9 | 578 | 284 |
| 64 `int` keys: construct + destroy | 12.2 | 5.70 | 11.5 | 22.3 |
| 64 `int` keys: get hit | 7.82 | 3.53 | 5.65 | 9.93 |
| 64 `int` keys: get miss | 8.47 | 3.52 | 5.11 | 9.91 |

## Intel Core i9-9980HK, macOS (x86-64), fastest of 3 runs

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 64 keys, 1 thread | 173 | 86.0 | 87.2 | 110 |
| 64 keys, 4 threads | 673 | 71.3 | 345 | 340 |
| 4096 keys, 1 thread | 380 | 127 | 124 | 141 |
| 4096 keys, 4 threads | 2318 | 73.0 | 494 | 439 |
| 100000 keys, 1 thread | 1049 | 434 | 396 | 447 |
| 100000 keys, 4 threads | 2855 | 125 | 1857 | 599 |
| 64 `int` keys: construct + destroy | 68.5 | 25.1 | 50.6 | 79.6 |
| 64 `int` keys: get hit | 21.1 | 10.9 | 17.4 | 37.5 |
| 64 `int` keys: get miss | 21.7 | 10.4 | 16.8 | 37.9 |

## What the rows show

- **One thread, string keys.** `absl` + `std::mutex` is the fastest or level with `Cache` at every size:
  6%, 9% and 16% faster on the M4 Pro at 64, 4096 and 100000 keys, and within 3% at 64 and 4096 keys on
  the i9 (10% faster at 100000). `Cache` is 2 to 5 times faster than `std::map` + `std::mutex`.
- **Four threads.** `Cache` takes a read lock with one atomic add, so its cost per `get` stays at 63 to 73 ns on
  the M4 Pro and 71 to 125 ns on the i9 as the table grows, while every comparator is
  1.5 to 32 times slower.
- **Small cache.** `Cache` is the fastest on construction and on `get`: 1.6 times faster than `absl` +
  `std::mutex` on a hit on the M4 Pro, 1.6 times on the i9.
