# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
generation_cache.hh   The whole library: templated Cache<Key, Value>.
test/test.cc          Runnable smoke test: hit/miss, clear(), stale-insert
                       rejection, and a threaded try_insert/clear race (run
                       under TSan/ASan in CI).
test/differential.cc   Differential test against a frozen reference cache;
                       CTest test `generation_cache_differential`.
test/exception_safety.cc  Injects throwing Key/Value copies and moves and
                       bad_alloc across several rehashes; checks a std::map
                       model after every failure; CTest test
                       `generation_cache_exception_safety`.
test/lock.cc           The reader-writer lock: readers share, a writer is
                       exclusive, a waiting writer holds new readers back,
                       nobody starves; CTest test `generation_cache_lock`, the
                       program to run under TSan.
test/first_insert.cc    The table allocates nothing until the first insert, then 4
                       slots, and doubles when full (at most 7/8 of the slots, and
                       never the last one); CTest test `generation_cache_first_insert`.
test/group.cc          `group::equal` against a byte loop for every byte value at
                       every position, with the empty and padding bytes; CTest test
                       `generation_cache_group`.
test/model.cc          The cache and the table against a std::unordered_map model plus
                       the generation rule over long random operation sequences, with
                       colliding and poorly mixed hashes, growth from nothing, the load
                       rule and probe lengths; CTest test `generation_cache_model`.
                       The group, model, exception_safety, first_insert and
                       differential tests are built twice, the second time with
                       GENERATION_CACHE_NO_SIMD (the portable 64-bit group), as
                       `generation_cache_<name>_nosimd`.
tools/mutants.py       Breaks the header in 29 ways and requires the tests to fail on
                       each (`python3 tools/mutants.py -j 2 [CXX]`).
test/seed.cc           Constructing a Cache cannot throw when the entropy source
                       does (GENERATION_CACHE_ENTROPY); CTest test
                       `generation_cache_seed`.
bench/bench.cc         Benchmark against a std::map + std::mutex cache and,
                       when abseil is found, absl::flat_hash_map behind a mutex and
                       a shared_mutex, with small-cache rows; it first checks that
                       every comparator returns Cache's results (`check` runs only
                       that; CTest test `generation_cache_bench_check`; `nomap` leaves
                       out the slow std::map rows). Target
                       `generation_cache_bench`, built with
                       GENERATION_CACHE_BUILD_BENCH=ON.
examples/demo.cc       A runnable tour (not a test): a warm cache, a source
                       change, a recompute.
CMakeLists.txt         Header-only INTERFACE target `generation_cache`
                       (+ alias generation_cache::generation_cache); no
                       FetchContent, no external dependency; CTest test
                       `generation_cache`.
LICENSE                MIT, Copyright (c) 2026 Germán Méndez Bravo.
README.md              What it is, install, usage, API, performance, what it isn't.
CHANGELOG.md           Dated changes.
ARCHITECTURE.md        Internal design: the race clear() defends against,
                       the reader-writer lock, the container, why no built-in size bound.
```

Header-only. There is no `.cc` to compile; `generation_cache.hh` is both the
declaration and the implementation. The CMake target is an `INTERFACE`
library purely for the include path and the C++ standard requirement.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

No network is needed. Expected output ends with `all generation-cache tests
passed`, exit 0. The test target is `generation_cache_test`; the registered
CTest name is `generation_cache`. The test and demo are only added when this
repo is the top-level project (`CMAKE_SOURCE_DIR STREQUAL
CMAKE_CURRENT_SOURCE_DIR`), so a consumer vendoring it via `FetchContent`
won't build them.

For the threaded race test, also run under TSan to actually catch a data
race, not just the deterministic assertions:

```sh
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ./build-tsan/generation_cache_test
```

**Docker note:** if the TSan binary crashes with `FATAL: ThreadSanitizer:
unexpected memory mapping ...` inside a container, that's a known
Docker+TSan+ASLR interaction, not a bug here -- confirmed by a bare
`#include <thread>` hello-world segfaulting identically under the same
flags. Work around it with `setarch $(uname -m) -R <binary>` (disables
ASLR) rather than chasing it as a code issue.

## Dependencies

None beyond the standard library (ARCHITECTURE.md lists the headers). C++17
is the floor (`std::optional`, `std::shared_lock`); nothing here needs C++20.
No FetchContent and no third-party library in the library target; the
benchmark alone uses abseil, found with `find_package` and optional.

## Conventions

- **C++17**, not 20 -- this is simple enough it doesn't need concepts,
  `<=>`, or anything newer, and C++17 is a lower bar for a consumer to
  clear. Don't raise it without a real reason.
- Tabs for indentation, double quotes in code, no em dashes in prose.
- MIT-licensed; keep the copyright header (Copyright (c) 2026 Germán Méndez
  Bravo) on source files.
- One `detail::rw_lock` guards the table and the generation counter
  together (see ARCHITECTURE.md's "One lock, shared for reads"). Readers take
  it shared; `try_insert` and `clear` take it exclusively. Don't split it into
  per-bucket locks: the generation check's race-freedom depends on one lock.
  `test/lock.cc` is the lock's own test; run it under ThreadSanitizer after any
  change to `rw_lock`, and keep a reader's re-add under the mutex (its
  correctness argument is in the header).
- Constructing a `Cache` does not throw. The hash seed comes from
  `detail::process_seed()`, which swallows a failing entropy source;
  `GENERATION_CACHE_ENTROPY` names the source (`test/seed.cc` makes it throw).

## Load-bearing invariants

- **`try_insert` must re-check the generation under the SAME mutex that
  `clear()` bumps it under.** This is the entire reason the class exists;
  breaking this reintroduces the exact race described in ARCHITECTURE.md
  (a stale compute resurrecting after a `clear()`).
- **`get_with_generation` must read the value and the generation under one
  lock acquisition, not two.** Taking the lock twice (once for the map,
  once for the counter) reopens a window for `clear()` to land in between,
  making the returned `(value, generation)` pair internally inconsistent.
- **`clear()` must both erase the map AND bump the generation, atomically
  with respect to any `try_insert`.** Doing one without the other (e.g. a
  "fast clear" that only bumps the generation and lazily drops entries)
  would need every read path to also re-check staleness per-entry, which
  this design deliberately avoids by making `clear()` a hard cutover.
- **`grow()` builds the whole new table before it touches the old one.**
  Never change `ctrl_`, `slots_`, `group_mask_`, `capacity_` or `room_` before every
  entry is placed, and never move entries unless both `Key` and `Value` are
  nothrow-move (copy otherwise). A throw mid-rehash must leave the cache as it
  was; `test/exception_safety.cc` fails if it does not.
- **The table always has an empty slot, and slots never empty.** A lookup
  stops at the first group with an empty slot, because an entry is placed in the
  first group of its probe sequence that has room and nothing erases one entry.
  `max_size()` keeps at least one slot empty (`capacity - max(1, capacity / 8)`),
  so a probe ends. Single-entry erase would need tombstones in the control
  bytes; do not add it without them. The control bytes are `ctrl_empty`
  (`0x80`), a tag (`0` to `0x7F`), or `ctrl_padding` (`0xFF`, past the last slot of
  a table smaller than a group): the padding must stay neither a tag nor empty.
- **A group test reads `width` control bytes at once.** Every table has at
  least `width` control bytes (`ctrl_bytes()`), and its groups are aligned
  multiples of `width`. `test/group.cc` checks `group::equal` on every byte
  value; run it in both builds (SIMD and `GENERATION_CACHE_NO_SIMD`).
- **`std::hash` is called again for every key on a growth** (nothing stores the
  hash), so a `grow()` that moves entries relies on `std::hash` not throwing.
- **Don't add an eviction policy (LRU/TTL/LFU/size cap) to this class.**
  That's a different, already-solved problem --
  [`lru-cache`](https://github.com/Kronuz/lru-cache) -- with a
  fundamentally different invalidation shape (rank individual entries,
  evict the worst-ranked one) than this class's (invalidate everything at
  once, in response to one external event). See this repo's own
  README "What it isn't" section and `lru-cache`'s own policy-callback
  design (its `OnDrop`/`OnGet` callbacks) if a ranked-eviction need shows
  up -- it's very likely the right tool already, not a reason to grow this
  one.

## Traps

- **A `try_insert` with a stale generation is not an error.** It's the
  expected, silent outcome of a benign race (a write landed while a reader
  was computing). Don't make it throw, log, or return an error code wired
  to anything user-visible by default -- a caller that wants to know can
  check the bool return value.
- **This is not a drop-in replacement for `lru-cache`.** If you're tempted
  to use this where you actually want size-bounded or recency-bounded
  eviction, you want `lru-cache` instead; see README's "What it isn't".

## Standalone vs. Detent

This was extracted from [Detent](https://github.com/Kronuz/Detent)'s
`detentd` distributor, where an ad hoc composed-diff cache (keyed by a
poller's last-known version, cleared on every accepted write) had exactly
this shape and this race. The extracted version here is written fresh and
generalized (`Key`/`Value` template parameters instead of
`std::uint64_t`/a Detent-specific patch type); it is not a verbatim copy
the way some other libraries in this collection are extractions of
pre-existing Xapiand code.

## CI

The shared cpp-libraries workflow runs on pushes, pull requests, manual dispatch and nightly. It builds and tests Linux GCC/Clang and ARM64, both macOS architectures and FreeBSD, plus Linux ASan/UBSan and TSan. Keep the threaded stale-insert race in the sanitizer jobs; a local smoke pass alone does not qualify changes to synchronization.
