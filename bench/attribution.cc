// Attribution harness: where does a get() at 100000 string keys spend its time?
// Each row times one piece of a get in isolation (key walk, std::hash, absl::Hash, lock pairs,
// hash_entries::find, the whole get with the value copy) over the same key walk as bench/bench.cc,
// once in random order and once sequentially. Not wired into CMake: build it by hand against any
// generation_cache.hh (the repo's, or an older commit's saved to a directory), with abseil for the rows
// that name it:
//   c++ -std=c++17 -O3 -DNDEBUG -DHAVE_ABSL=1 -I<dir with generation_cache.hh> bench/attribution.cc \
//       -labsl_hash ... -o attribution        (or add it to a CMake target that links absl::flat_hash_map absl::hash)
//   ./attribution [n]                          (default 100000)
// Run it in a `quiet strict` hold, and check the binary's architecture with `file` first.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#ifdef HAVE_ABSL
#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#endif

#include "generation_cache.hh"

using Str = std::string;
static volatile size_t sink;

template <class F>
static double best(F&& f, double ops, int reps = 7) {
	f();
	double b = 1e18;
	for (int i = 0; i < reps; ++i) {
		auto a = std::chrono::steady_clock::now();
		f();
		auto e = std::chrono::steady_clock::now();
		b = std::min(b, std::chrono::duration<double, std::nano>(e - a).count() / ops);
	}
	return b;
}

int main(int argc, char** argv) {
	size_t n = argc > 1 ? std::stoul(argv[1]) : 100000;
	const int iters = 1000000;
	std::vector<Str> keys(n);
	for (size_t i = 0; i < n; ++i) keys[i] = "/api/v1/resource/" + std::to_string(i) + "?etag=abc";
	std::vector<size_t> order(iters);
	{
		size_t i = 0;
		for (int k = 0; k < iters; ++k) { i = (i * 2654435761u + 1) % n; order[k] = i; }
	}
	std::vector<size_t> seq(iters);
	for (int k = 0; k < iters; ++k) seq[k] = k % n;

	generation_cache::detail::hash_entries<Str, Str> tab;
	generation_cache::Cache<Str, Str> cache;
	for (auto& k : keys) { tab.insert_or_assign(k, "value-" + k); cache.try_insert(k, "value-" + k, cache.generation()); }
#ifdef HAVE_ABSL
	absl::flat_hash_map<Str, Str> am;
	for (auto& k : keys) am[k] = "value-" + k;
#endif
	std::mutex mu;
	generation_cache::detail::rw_lock rw;
	std::hash<Str> sh;

	auto row = [&](const char* name, auto&& fn, const std::vector<size_t>& ord) {
		double t = best([&] { size_t s = 0; for (int k = 0; k < iters; ++k) s += fn(keys[ord[k]]); sink = s; }, iters);
		std::printf("  %-34s %7.1f ns\n", name, t);
	};
	for (int pass = 0; pass < 2; ++pass) {
		const auto& ord = pass == 0 ? order : seq;
		std::printf("n=%zu, %s key walk\n", n, pass == 0 ? "random" : "sequential");
		row("walk only (key size)", [&](const Str& k) { return k.size(); }, ord);
		row("std::hash<string>", [&](const Str& k) { return sh(k); }, ord);
#ifdef HAVE_ABSL
		row("absl::Hash<string>", [&](const Str& k) { return absl::Hash<Str>{}(k); }, ord);
#endif
		row("std::mutex lock+unlock", [&](const Str& k) { mu.lock(); size_t r = k.size(); mu.unlock(); return r; }, ord);
		row("rw_lock shared lock+unlock", [&](const Str& k) { rw.lock_shared(); size_t r = k.size(); rw.unlock_shared(); return r; }, ord);
		row("hash_entries::find", [&](const Str& k) { const Str* v = tab.find(k); return v ? v->size() : 0; }, ord);
#ifdef HAVE_ABSL
		row("absl find (no lock)", [&](const Str& k) { auto it = am.find(k); return it == am.end() ? 0 : it->second.size(); }, ord);
#endif
		row("Cache::get (copy value)", [&](const Str& k) { auto v = cache.get(k); return v ? v->size() : 0; }, ord);
#ifdef HAVE_ABSL
		row("absl+mutex get (copy value)", [&](const Str& k) { std::lock_guard<std::mutex> l(mu); auto it = am.find(k); if (it == am.end()) return size_t(0); Str c = it->second; return c.size(); }, ord);
#endif
	}
	return 0;
}
