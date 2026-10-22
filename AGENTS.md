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
examples/demo.cc       A runnable tour (not a test): a warm cache, a source
                       change, a recompute.
CMakeLists.txt         Header-only INTERFACE target `generation_cache`
                       (+ alias generation_cache::generation_cache); no
                       FetchContent, no external dependency; CTest test
                       `generation_cache`.
LICENSE                MIT, Copyright (c) 2026 Germán Méndez Bravo.
README.md              What it is, install, usage, API, what it isn't.
ARCHITECTURE.md        Internal design: the race clear() defends against,
                       why one mutex, why no built-in size bound.
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

None beyond the standard library: `<map>`, `<mutex>`, `<cstdint>`,
`<optional>`, `<utility>`. C++17 is the floor (`std::optional`); nothing
here needs C++20. No FetchContent, no third-party library.

## Conventions

- **C++17**, not 20 -- this is simple enough it doesn't need concepts,
  `<=>`, or anything newer, and C++17 is a lower bar for a consumer to
  clear. Don't raise it without a real reason.
- Tabs for indentation, double quotes in code, no em dashes in prose.
- MIT-licensed; keep the copyright header (Copyright (c) 2026 Germán Méndez
  Bravo) on source files.
- One mutex, guarding the map and the generation counter together (see
  ARCHITECTURE.md's "why one mutex" section). Don't split it into a
  `shared_mutex` or per-bucket locking without re-reading why that was
  deliberately avoided -- the generation-check race-freedom depends on it.

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
