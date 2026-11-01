// The table's first allocation is small: an empty cache holds nothing, the first insert allocates
// 4 slots, and growth doubles from there, so a cache that holds a few entries pays for a few.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

#include "generation_cache.hh"

template <typename Table, typename MakeKey>
static void check(const char* name, MakeKey key) {
	Table t;
	assert(t.capacity() == 0 && t.size() == 0);  // nothing is allocated before the first insert
	t.insert_or_assign(key(1), key(1));
	assert(t.capacity() <= 4);  // the first insert allocates 4 slots at most
	for (int n = 2; n <= 5000; ++n) {
		t.insert_or_assign(key(n), key(n));
		const std::size_t c = t.capacity();
		assert((c & (c - 1)) == 0);          // a power of two
		assert(t.size() <= c - std::max<std::size_t>(1, c / 8));  // load at most 7/8, and a slot is always empty
		assert(c <= std::max<std::size_t>(4, 4 * t.size()));  // and never more than doubled too early
	}
	for (int n = 1; n <= 5000; ++n) assert(t.find(key(n)) && *t.find(key(n)) == key(n));
	t.clear();
	assert(t.capacity() == 0 && t.size() == 0);  // clear returns to the empty state
	t.insert_or_assign(key(7), key(7));
	assert(t.capacity() <= 4);
	std::printf("first insert OK: %s\n", name);
}

int main() {
	using generation_cache::detail::hash_entries;
	check<hash_entries<int, int>>("int", [](int n) { return n; });
	check<hash_entries<std::string, std::string>>("string", [](int n) { return "key-number-" + std::to_string(n) + "-long-enough-to-heap"; });
	std::puts("all first-insert tests passed");
	return 0;
}
