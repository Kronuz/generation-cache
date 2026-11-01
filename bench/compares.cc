// Counts key comparisons per successful and failed lookup (comparisons that a tag match costs and
// a wider tag would save) and, with -DGC_HAVE_PROBES=1 (the table's probes() diagnostic), the groups
// examined per lookup. Deterministic: no timing. Build by hand against a generation_cache.hh:
//   c++ -std=c++17 -O2 -DGC_HAVE_PROBES=1 -I<dir with generation_cache.hh> bench/compares.cc -o compares && ./compares
#include <cstdio>
#include <string>
#include <vector>

#include "generation_cache.hh"

static unsigned long compares = 0;
struct CStr {
	std::string s;
	bool operator==(const CStr& o) const { ++compares; return s == o.s; }
};
namespace std {
	template <> struct hash<CStr> { size_t operator()(const CStr& k) const { return std::hash<std::string>{}(k.s); } };
}

int main(int argc, char** argv) {
	for (size_t n : {64ul, 1000ul, 4096ul, 16384ul, 100000ul, 1000000ul}) {
		std::vector<CStr> keys(n), absent(n);
		for (size_t i = 0; i < n; ++i) {
			keys[i].s = "/api/v1/resource/" + std::to_string(i) + "?etag=abc";
			absent[i].s = "/api/v1/missing/" + std::to_string(i) + "?etag=abc";
		}
		generation_cache::detail::hash_entries<CStr, int> t;
		for (auto& k : keys) t.insert_or_assign(k, 1);
		compares = 0;
		for (auto& k : keys) (void)t.find(k);
		const double hit = double(compares) / n;
		compares = 0;
		for (auto& k : absent) (void)t.find(k);
		const double miss = double(compares) / n;
		double groups = 0;
#ifdef GC_HAVE_PROBES
		for (auto& k : keys) groups += double(t.probes(k));
		groups /= n;
#endif
		std::printf("n=%-8zu capacity %-8zu load %.2f  compares/hit %.3f  compares/miss %.3f  groups/hit %.3f\n", n, t.capacity(), double(n) / t.capacity(), hit, miss, groups);
	}
	return argc > 99;
}
