# Changelog

All notable changes to `generation-cache`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-10-31

### Fixed

- A throw during the hash table's rehash lost entries. `grow()` moved the old
  slots into the live table one at a time, so when a `Key` or `Value` move or
  copy threw, the table was left half filled, the remaining entries were
  destroyed with the old vector, and a slot could be left with its hash set and
  an empty `optional` that a later `get` dereferenced. `grow()` now builds the
  next table in a local vector and swaps it in at the end, moving entries only
  when `Key` and `Value` are nothrow-move-constructible and copying them
  otherwise. Oracle: a `std::map` model checked after every injected failure
  (`test/exception_safety.cc`). Results change only on the throwing paths.
- A `Key` with only `operator<` (the `std::map` adapter) kept a
  default-constructed entry when `Value`'s move assignment threw, because it
  assigned through `m_[key]`. It uses `insert_or_assign` now.
- `try_insert` has the strong exception guarantee. A throwing `Key` copy used
  to leave a hash-table slot with its hash set and an empty `optional`, so a
  later probe dereferenced it (review finding CR1). The entry is now
  constructed before the slot's hash and the size are published (`89baa5b`).

### Changed

- Readers take a one-word reader-writer lock (`detail::rw_lock`) instead of
  `std::shared_mutex`: an atomic add to enter, an atomic subtract to leave, and a
  `std::mutex` that only writers and the readers they hold back use. On a short critical
  section `std::shared_mutex` was most of the cost of a lookup (libc++ takes two mutex round
  trips per shared lock), and `get` on a 64-entry cache was slower than the `std::mutex` it
  replaced (audit finding CA4). A waiting writer now holds new readers back on every
  platform, where the fairness of `std::shared_mutex` was the platform's (glibc prefers
  readers), so continuous readers cannot starve `try_insert` or `clear`.
- The table's first insert allocates 4 slots, not 16.
- The hash table folds a per-cache random seed into `std::hash` before mixing,
  so a key set cannot be crafted offline to collide in every cache (CR4).
- The README and header document `std::shared_mutex` fairness (CR4).
- The hash seed is drawn once per process, not by two `std::random_device` draws in every
  cache: a cache's construction was dominated by them (audit finding CA4). A table varies the
  process seed by its own address. Constructing a `Cache` cannot throw: an entropy source
  that fails costs seed quality (the clock, the thread id and an address are mixed in), never
  an exception (the system review note). `GENERATION_CACHE_ENTROPY` names the source, which is
  how `test/seed.cc` makes it throw.
- The README and ARCHITECTURE.md no longer say a key set "cannot be crafted offline to
  collide" (the CR4 bullet above made the same claim): the seed spreads the table indexes of
  keys whose `std::hash` values differ, and keys with equal `std::hash` values collide in
  every cache.

### Added

- `test/first_insert.cc`: the table allocates nothing until the first insert, then 4 slots at
  most, and keeps a power-of-two capacity with load at most 0.5 while it grows.
- `test/seed.cc`: a `Cache` is constructed, filled across several rehashes and cleared while
  the entropy source throws, and the default constructor is `static_assert`ed nothrow.
- `test/lock.cc`: readers share the lock, a writer is exclusive, a waiting writer holds new
  readers back, and neither side starves; the program to run under ThreadSanitizer.
- Small-cache rows in `bench/bench.cc` (construct and destroy, get hit and miss with 64 `int`
  keys). `generation_cache_bench` takes sizes and `small` as arguments.

### Performance

The rehash fix leaves the read path alone: `get`, `get_with_generation` and
`find` compile to the same machine code (clang -O3). A/B against the previous
header on x86_64 Linux (g++ 11.2 -O3, fixed hash seed, `string` keys,
n = 100000, 12 interleaved runs each, a copy of the old binary as the A/A
control), mean ns: `get` on 1 thread 393 old, 392 new (old copy 392); 4 threads
118 old, 121 new (old copy 127); fill 356 old, 354 new. The difference is inside
the A/A spread.

### Performance of the lock and the seed

The audit's probe (`extra_bench.cc`), ns, mean of two passes: `start` is 00762bd (a `std::map`
behind a `std::mutex`), `before` is 3a0a744, `after` is this change.

| row | arm64 start | before | after | x86_64 start | before | after |
|---|---|---|---|---|---|---|
| construct and destroy | 12.2 | 80.9 | 5.80 | 54.0 | 291 | 25.1 |
| construct, insert once, destroy | 35.9 | 128 | 28.4 | 134 | 492 | 134 |
| 64 `int` keys: get hit | 7.83 | 11.9 | 3.63 | 18.4 | 37.7 | 10.0 |
| 64 `int` keys: get miss | 8.43 | 11.7 | 3.81 | 20.4 | 37.7 | 10.4 |
| fill 256, then clear (per insert) | 30.2 | 30.5 | 12.2 | 92.9 | 81.3 | 45.0 |

String keys of about 30 bytes, `get` hit, n = 100000, ns per op (`generation_cache_bench`, mean of two
passes; `before` is 3a0a744):

| row | arm64 before | after | x86_64 before | after |
|---|---|---|---|---|
| 100000 keys, 1 thread | 315 | 147 | 481 | 440 |
| 100000 keys, 4 threads | 333 | 84.4 | 593 | 134 |

Construct, insert once and destroy was the one probe row that got slower than the `std::map`
it replaced (36 to 38 ns on arm64, 134 to 192 ns on x86_64; 3a0a744 took 128 and 492): the
first insert allocated and initialized 16 slots, 384 bytes for `int` keys, and on macOS x86_64
`malloc` takes 82 ns above 256 bytes and 36 below. It allocates 4 slots now, and the row takes
28 ns on arm64 and 134 ns on x86_64, level with the `std::map` on x86_64 and 22% under it on
arm64. A cache that grows past 2 entries doubles from there, and the other rows do not move
(fill 256, then clear, per insert: 11.3 and 11.5 ns on arm64, 46.5 and 46.6 on x86_64, against
30 and 93 for the `std::map`; get hit and miss on 64 keys within 3%).

The gap to `absl::flat_hash_map` at 100000 keys on one thread was the lock. The cache now
matches abseil behind a `std::shared_mutex` (arm64 147 against 194 ns, x86_64 440 against 445)
and stays within 9% of it behind a `std::mutex` (135 and 410), the spread of that row being up
to 20%. With one lock under both tables (a copy of each in the benchmark's decomposition
harness) this table is 8% faster than abseil's on arm64 (166 against 181 ns) and 9% slower on
x86_64 (449 against 411). A tag-byte layout that probes one byte per slot was no faster (179 to
183 ns on arm64, 456 to 463 on x86_64), so the table is unchanged.

## 2026-10-30

### Changed

- A `Key` with `std::hash` and `operator==` lives in a small open-addressing
  table instead of `std::map`. A `Key` with only `operator<` still uses
  `std::map` (`715f2af`).
- `get`, `get_with_generation`, `generation` and `size` take a
  `std::shared_mutex` in shared mode, so readers run in parallel. They used to
  take one exclusive `std::mutex`. `try_insert` and `clear` stay exclusive, so
  the stale-generation check is unchanged.

### Added

- `test/differential.cc`, a differential test against a frozen copy of the
  previous cache.
- `generation_cache_bench`, a benchmark against `std::mutex` and
  `std::shared_mutex` plus `absl::flat_hash_map` (bench target only).

### Performance

Apple arm64, string keys, `get` hit, n = 100000, shared machine (absolute
numbers are noisy), ns per op:

| threads | before | after | `shared_mutex` + `absl::flat_hash_map` |
|---|---|---|---|
| 1 | 446 | 116 | 99 |
| 4 | 2141 | 294 | 284 |
