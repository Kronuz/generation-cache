// A runnable tour, not a test: models the motivating case -- a server
// composes an expensive Value from a Key against some shared state, caches
// it, and invalidates everything cached whenever that shared state changes.
#include "../generation_cache.hh"

#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

namespace {

// Stands in for "recompute the expensive thing" -- a real caller would
// parse/merge/serialize here instead of just formatting a string.
std::string expensive_compute(int key, int source_version) {
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(5ms); // pretend this takes a moment
	return "value(" + std::to_string(key) + ") as of source v" + std::to_string(source_version);
}

} // namespace

int main() {
	generation_cache::Cache<int, std::string> cache;
	int source_version = 1;

	auto fetch = [&](int key) {
		auto lookup = cache.get_with_generation(key);
		if (lookup.value) {
			std::printf("key %d: cache HIT  -> %s\n", key, lookup.value->c_str());
			return;
		}
		auto value = expensive_compute(key, source_version);
		cache.try_insert(key, value, lookup.generation);
		std::printf("key %d: cache MISS -> %s (now cached)\n", key, value.c_str());
	};

	std::puts("-- three readers ask for key 42, same source version --");
	fetch(42); // miss, computes and caches
	fetch(42); // hit, reuses the cached value
	fetch(42); // hit again

	std::puts("\n-- the source changes (e.g. a new write was accepted) --");
	++source_version;
	cache.clear();

	std::puts("-- key 42 is asked for again, now against the NEW source version --");
	fetch(42); // miss again: the old cached value was for the old version
	fetch(42); // hit: fresh value for the new version, now cached

	std::printf("\nfinal cache size: %zu entr%s\n", cache.size(), cache.size() == 1 ? "y" : "ies");
	return 0;
}
