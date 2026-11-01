cmake_minimum_required(VERSION 3.14)
project(generation_cache LANGUAGES CXX)

# --- Library target ---
# Header-only: the whole implementation is a templated class in
# generation_cache.hh, so there's nothing to compile into a translation
# unit. The target exists for the include path and the C++ standard
# requirement, matching the sibling libraries' interface-target shape
# (e.g. cpp-btree::headers).
add_library(generation_cache INTERFACE)
target_include_directories(generation_cache INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(generation_cache INTERFACE cxx_std_17)

# Alias so consumers can link `generation_cache::generation_cache` whether
# they add this via add_subdirectory or FetchContent.
add_library(generation_cache::generation_cache ALIAS generation_cache)

# Only build the test and demo when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  find_package(Threads REQUIRED)

  add_executable(generation_cache_test test/test.cc)
  target_link_libraries(generation_cache_test PRIVATE generation_cache Threads::Threads)
  add_test(NAME generation_cache COMMAND generation_cache_test)

  # Differential test against a frozen copy of the original std::map + std::mutex cache.
  add_executable(generation_cache_differential test/differential.cc)
  target_link_libraries(generation_cache_differential PRIVATE generation_cache Threads::Threads)
  add_test(NAME generation_cache_differential COMMAND generation_cache_differential)

  # Strong exception guarantee: throwing Key/Value copies and moves and injected
  # bad_alloc at every point of an insert sequence that crosses several rehashes.
  add_executable(generation_cache_exception_safety test/exception_safety.cc)
  target_link_libraries(generation_cache_exception_safety PRIVATE generation_cache Threads::Threads)
  add_test(NAME generation_cache_exception_safety COMMAND generation_cache_exception_safety)

  # The reader-writer lock: readers share, a writer is exclusive, a waiting writer holds new readers
  # back, nobody starves. Also the program to run under ThreadSanitizer.
  add_executable(generation_cache_lock test/lock.cc)
  target_link_libraries(generation_cache_lock PRIVATE generation_cache Threads::Threads)
  add_test(NAME generation_cache_lock COMMAND generation_cache_lock)

  # Constructing a Cache cannot throw, even when the entropy source for the hash seed does.
  add_executable(generation_cache_seed test/seed.cc)
  target_link_libraries(generation_cache_seed PRIVATE generation_cache Threads::Threads)
  add_test(NAME generation_cache_seed COMMAND generation_cache_seed)

  # The table's first allocation is small and growth doubles from it.
  add_executable(generation_cache_first_insert test/first_insert.cc)
  target_link_libraries(generation_cache_first_insert PRIVATE generation_cache)
  add_test(NAME generation_cache_first_insert COMMAND generation_cache_first_insert)

  # The control-byte group test (SSE2, NEON or the 64-bit word, whichever the platform selects).
  add_executable(generation_cache_group test/group.cc)
  target_link_libraries(generation_cache_group PRIVATE generation_cache)
  add_test(NAME generation_cache_group COMMAND generation_cache_group)

  # The cache and the table against a std::unordered_map model: long random operation sequences,
  # colliding and poorly mixed hashes, growth from nothing, the load rule, probe lengths.
  add_executable(generation_cache_model test/model.cc)
  target_link_libraries(generation_cache_model PRIVATE generation_cache)
  add_test(NAME generation_cache_model COMMAND generation_cache_model)

  # The same tests with the portable 64-bit group, which the SIMD groups replace on x86-64 and arm64.
  foreach(nosimd_test group model exception_safety first_insert differential)
    add_executable(generation_cache_${nosimd_test}_nosimd test/${nosimd_test}.cc)
    target_link_libraries(generation_cache_${nosimd_test}_nosimd PRIVATE generation_cache Threads::Threads)
    target_compile_definitions(generation_cache_${nosimd_test}_nosimd PRIVATE GENERATION_CACHE_NO_SIMD=1)
    add_test(NAME generation_cache_${nosimd_test}_nosimd COMMAND generation_cache_${nosimd_test}_nosimd)
  endforeach()

  # Benchmark (off by default): Cache against a std::map behind a std::mutex and, when abseil is
  # found, against absl::flat_hash_map behind a mutex and a shared_mutex. The comparators are used
  # only by this target. `generation_cache_bench check` verifies, with no timing, that each one
  # returns the same results as Cache for every key it looks up.
  #   cmake -B build -DGENERATION_CACHE_BUILD_BENCH=ON && cmake --build build --target generation_cache_bench
  option(GENERATION_CACHE_BUILD_BENCH "Build bench/bench.cc" OFF)
  if(GENERATION_CACHE_BUILD_BENCH)
    add_executable(generation_cache_bench bench/bench.cc)
    target_link_libraries(generation_cache_bench PRIVATE generation_cache Threads::Threads)
    target_compile_options(generation_cache_bench PRIVATE -O3 -DNDEBUG)
    find_package(absl QUIET)
    if(absl_FOUND)
      target_compile_definitions(generation_cache_bench PRIVATE HAVE_ABSL=1)
      target_link_libraries(generation_cache_bench PRIVATE absl::flat_hash_map)
    endif()
    add_test(NAME generation_cache_bench_check COMMAND generation_cache_bench check)
  endif()

  # A runnable tour of the library (not a test): build & run
  # ./build/generation_cache_demo
  add_executable(generation_cache_demo examples/demo.cc)
  target_link_libraries(generation_cache_demo PRIVATE generation_cache Threads::Threads)
endif()
