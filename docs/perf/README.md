# generation-cache performance

`bench/bench.cc`, ns per operation, best of 5, mean of two passes, `-O3 -DNDEBUG`.
String rows use keys of about 30 bytes and time a `get` hit on 1 and on 4
threads. The small-cache rows hold 64 `int` keys on 1 thread. The comparators
are a `std::map` behind a `std::mutex` and `absl::flat_hash_map` behind a
`std::mutex` or a `std::shared_mutex`; the abseil version is not recorded with
these runs.

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
| 100000 keys, 1 thread | 723 | 147 | 135 | 194 |
| 100000 keys, 4 threads | 2569 | 84.4 | 622 | 293 |
| 64 `int` keys: construct + destroy | 12.5 | 5.7 | 11.5 | 22.4 |
| 64 `int` keys: get hit | 7.9 | 3.5 | 5.4 | 10.2 |
| 64 `int` keys: get miss | 8.6 | 3.5 | 5.1 | 10.1 |

## Intel Core i9-9980HK (x86_64), Apple clang 17

| row | `std::map` + `std::mutex` | `Cache` | `absl` + `std::mutex` | `absl` + `std::shared_mutex` |
|---|---|---|---|---|
| 100000 keys, 1 thread | 1098 | 440 | 410 | 445 |
| 100000 keys, 4 threads | 2713 | 134 | 1789 | 574 |
| 64 `int` keys: construct + destroy | 62.6 | 24.5 | 50.1 | 81.1 |
| 64 `int` keys: get hit | 19.1 | 10.6 | 17.1 | 36.7 |
| 64 `int` keys: get miss | 20.9 | 10.7 | 17.3 | 36.4 |
