# Changelog

All notable changes to `generation-cache`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-10-31

### Fixed

- `try_insert` has the strong exception guarantee. A throwing `Key` copy used
  to leave a hash-table slot with its hash set and an empty `optional`, so a
  later probe dereferenced it (review finding CR1). The entry is now
  constructed before the slot's hash and the size are published (`89baa5b`).

### Changed

- The hash table folds a per-cache random seed into `std::hash` before mixing,
  so a key set cannot be crafted offline to collide in every cache (CR4).
- The README and header document `std::shared_mutex` fairness (CR4).

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
