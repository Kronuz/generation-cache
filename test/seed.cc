// Constructing a Cache never throws, even when the operating system's entropy source does. The
// source is replaced before the header is included, in its own program, because the process
// seed is drawn once, on first use.
#undef NDEBUG
#include <cstdio>
#include <stdexcept>
#include <string>

struct ThrowingEntropy {
	ThrowingEntropy() { throw std::runtime_error("no entropy source"); }
	unsigned operator()() { return 0; }
};
#define GENERATION_CACHE_ENTROPY ThrowingEntropy

#include <cassert>
#include "generation_cache.hh"

using IntCache = generation_cache::Cache<int, int>;

int main() {
	// The seed still comes out: nonzero and the same for the whole process.
	const auto seed = generation_cache::detail::process_seed();
	assert(seed != 0 && seed == generation_cache::detail::process_seed());

	// Caches built on it work: a few hundred keys, several rehashes, a clear, and a second cache.
	IntCache a;
	for (int i = 0; i < 500; ++i) assert(a.try_insert(i, i * 3, a.generation()));
	for (int i = 0; i < 500; ++i) assert(a.get(i).value() == i * 3);
	assert(!a.get(500).has_value() && a.size() == 500);
	a.clear();
	assert(a.size() == 0 && !a.get(7).has_value());
	generation_cache::Cache<std::string, int> b;
	assert(b.try_insert("x", 1, b.generation()) && b.get("x").value() == 1);
	std::puts("generation-cache seed OK: construction survives a throwing entropy source");
	return 0;
}
