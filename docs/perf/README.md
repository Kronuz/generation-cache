# generation-cache performance

`bench/bench.cc`, ns per operation, best of 5, mean of two passes, then the mean of two
runs, built with `-O3 -DNDEBUG`.
String rows use keys of about 30 bytes and time a `get` hit on 1 and on 4
threads. The small-cache rows hold 64 `int` keys on 1 thread. The comparators
are a `std::map` behind a `std::mutex` and `absl::flat_hash_map` from abseil 20260817
behind a `std::mutex` or a `std::shared_mutex`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGENERATION_CACHE_BUILD_BENCH=ON
cmake --build build --target generation_cache_bench
./build/generation_cache_bench [n ...] [small]
```

The `absl` columns appear when abseil is installed (`find_package(absl)`, used
only by the benchmark). Arguments select the rows: sizes run only those string
rows, and `small` adds the small-cache rows. With no arguments it runs 64, 4096
and 100000, then the small-cache rows.

## Apple M4 Pro (arm64), Apple clang 17

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 100000 keys, 1 thread | 536 | 109 | 88.6 | 132 |
| 100000 keys, 4 threads | 2039 | 61.6 | 459 | 277 |
| 64 `int` keys: construct + destroy | 12.4 | 5.8 | 11.5 | 22.6 |
| 64 `int` keys: get hit | 7.9 | 3.5 | 5.4 | 10.2 |
| 64 `int` keys: get miss | 8.7 | 3.6 | 5.0 | 10.0 |

## Intel Core i9-9980HK (x86_64), Apple clang 17

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 100000 keys, 1 thread | 1081 | 434 | 398 | 429 |
| 100000 keys, 4 threads | 2751 | 119 | 1634 | 640 |
| 64 `int` keys: construct + destroy | 64.2 | 24.1 | 50.3 | 77.8 |
| 64 `int` keys: get hit | 19.1 | 10.2 | 16.7 | 36.1 |
| 64 `int` keys: get miss | 21.2 | 10.5 | 16.6 | 37.4 |
