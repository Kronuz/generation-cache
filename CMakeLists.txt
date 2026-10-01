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

  # A runnable tour of the library (not a test): build & run
  # ./build/generation_cache_demo
  add_executable(generation_cache_demo examples/demo.cc)
  target_link_libraries(generation_cache_demo PRIVATE generation_cache Threads::Threads)
endif()
