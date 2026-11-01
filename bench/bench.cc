// Benchmark: generation_cache::Cache against a hand-written std::map behind a std::mutex
// and, when abseil is found (HAVE_ABSL), against absl::flat_hash_map behind a mutex / shared_mutex.
// get() hit with string keys, 1 thread and 4 threads, at three sizes; then the costs of
// a small cache: construct + destroy, and get hit and miss with 64 int keys. ns per op, best of 5.
// Before timing, every comparator is checked to return the same value as Cache for every key it
// looks up (hits and misses); a mismatch aborts.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef HAVE_ABSL
#include <absl/container/flat_hash_map.h>
#endif

#include "generation_cache.hh"

using Str = std::string;

static volatile size_t sink;
static bool check_only = false;  // `check`: verify results, time nothing

template <class F>
static double best_ns(F&& f, double ops, int reps = 7) {
	f();
	double best = 1e18;
	for (int i = 0; i < reps; ++i) {
		auto a = std::chrono::steady_clock::now();
		f();
		auto b = std::chrono::steady_clock::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count() / ops);
	}
	return best;
}

template <class K, class V>
class MapCache {
	mutable std::mutex m_;
	std::map<K, V> e_;
public:
	std::optional<V> get(const K& k) const {
		std::lock_guard<std::mutex> l(m_);
		auto it = e_.find(k);
		if (it == e_.end()) return std::nullopt;
		return it->second;
	}
	void put(const K& k, V v) { std::lock_guard<std::mutex> l(m_); e_[k] = std::move(v); }
	std::size_t size() const { std::lock_guard<std::mutex> l(m_); return e_.size(); }
};

#ifdef HAVE_ABSL
template <class M, class K, class V>
class AbslCache {
	mutable M m_;
	absl::flat_hash_map<K, V> e_;
public:
	std::optional<V> get(const K& k) const {
		if constexpr (std::is_same_v<M, std::mutex>) {
			std::lock_guard<M> l(m_);
			auto it = e_.find(k);
			if (it == e_.end()) return std::nullopt;
			return it->second;
		} else {
			std::shared_lock<M> l(m_);
			auto it = e_.find(k);
			if (it == e_.end()) return std::nullopt;
			return it->second;
		}
	}
	void put(const K& k, V v) { std::unique_lock<M> l(m_); e_[k] = std::move(v); }
	std::size_t size() const {
		if constexpr (std::is_same_v<M, std::mutex>) {
			std::lock_guard<M> l(m_);
			return e_.size();
		} else {
			std::shared_lock<M> l(m_);
			return e_.size();
		}
	}
};

#endif

template <class K, class V>
struct GenCache {
	generation_cache::Cache<K, V> c;
	std::optional<V> get(const K& k) const { return c.get(k); }
	void put(const K& k, V v) { c.try_insert(k, std::move(v), c.generation()); }
	std::size_t size() const { return c.size(); }
};

// Aborts unless c returns the same result as a Cache holding the same entries for every key
// in `probe`: the same hit or miss and, on a hit, the same value.
template <class C, class K, class V>
static void check_same(const char* name, const C& c, const GenCache<K, V>& ref, const std::vector<K>& probe) {
	for (const K& k : probe) {
		if (c.get(k) != ref.get(k)) {
			std::fprintf(stderr, "MISMATCH: %s and Cache return different results for a key\n", name);
			std::abort();
		}
	}
}

template <class C>
static double mt(const C& c, const std::vector<std::string>& keys, int nthreads, int iters) {
	auto a = std::chrono::steady_clock::now();
	std::vector<std::thread> th;
	for (int t = 0; t < nthreads; ++t)
		th.emplace_back([&, t] {
			size_t s = 0, i = t;
			for (int n = 0; n < iters; ++n) {
				i = (i * 2654435761u + 1) % keys.size();
				auto v = c.get(keys[i]);
				s += v ? v->size() : 0;
			}
			sink = s;
		});
	for (auto& x : th) x.join();
	return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count() / (double(nthreads) * iters);
}

template <class C>
static void row(const char* name, size_t n, const std::vector<std::string>& keys) {
	C c;
	for (auto& k : keys) c.put(k, "value-" + k);
	GenCache<Str, Str> ref;
	for (auto& k : keys) ref.put(k, "value-" + k);
	std::vector<std::string> probe = keys;
	probe.push_back("/not/a/key");
	check_same(name, c, ref, probe);
	if (check_only) return;
	double t1 = 1e18, t4 = 1e18;
	for (int i = 0; i < 5; ++i) {
		t1 = std::min(t1, mt(c, keys, 1, 500000));
		t4 = std::min(t4, mt(c, keys, 4, 300000));
	}
	std::printf("  n=%-7zu %-26s 1 thread %7.1f ns   4 threads %7.1f ns/op\n", n, name, t1, t4);
}

// A small cache: what building one costs, and a get that is over almost as soon as it starts.
template <class C>
static void small_row(const char* name) {
	C c;
	for (int i = 0; i < 64; ++i) c.put(i, i);
	GenCache<int, int> ref;
	for (int i = 0; i < 64; ++i) ref.put(i, i);
	std::vector<int> probe(128);
	for (int i = 0; i < 128; ++i) probe[i] = i;
	check_same(name, c, ref, probe);
	if (check_only) return;
	double ctor = best_ns([] { for (int i = 0; i < 20000; ++i) { C c; sink = c.size(); } }, 20000);
	double hit = best_ns([&] { size_t s = 0; for (int r = 0; r < 4000; ++r) for (int i = 0; i < 64; ++i) s += c.get(i).value_or(0); sink = s; }, 64.0 * 4000);
	double miss = best_ns([&] { size_t s = 0; for (int r = 0; r < 4000; ++r) for (int i = 64; i < 128; ++i) s += c.get(i).has_value(); sink = s; }, 64.0 * 4000);
	std::printf("  %-26s construct+destroy %6.1f ns   get hit %6.2f ns   get miss %6.2f ns\n", name, ctor, hit, miss);
}

// usage: generation_cache_bench [n ...] [small] [check]
// No arguments: string keys at n = 64, 4096 and 100000, then the small-cache rows.
// Sizes alone run only those string rows; `small` adds the small-cache rows.
// `check` only verifies that every comparator returns the same results as Cache, and times nothing.
int main(int argc, char** argv) {
	std::vector<size_t> sizes;
	bool small = false, defaults = true;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "check") check_only = true;
		else if (a == "small") small = true, defaults = false;
		else sizes.push_back(std::stoul(a)), defaults = false;
	}
	if (defaults) sizes = {64, 4096, 100000}, small = true;
	for (size_t n : sizes) {
		std::vector<std::string> keys(n);
		for (size_t i = 0; i < n; ++i) keys[i] = "/api/v1/resource/" + std::to_string(i) + "?etag=abc";
		row<MapCache<Str, Str>>("std::map + std::mutex", n, keys);
		row<GenCache<Str, Str>>("Cache", n, keys);
#ifdef HAVE_ABSL
		row<AbslCache<std::mutex, Str, Str>>("absl + std::mutex", n, keys);
		row<AbslCache<std::shared_mutex, Str, Str>>("absl + std::shared_mutex", n, keys);
#endif
	}
	if (!small) {
		if (check_only) std::puts("generation_cache_bench: every comparator returns the same results as Cache");
		return 0;
	}
	if (!check_only) std::printf("small cache, int keys, 1 thread\n");
	small_row<MapCache<int, int>>("std::map + std::mutex");
	small_row<GenCache<int, int>>("Cache");
#ifdef HAVE_ABSL
	small_row<AbslCache<std::mutex, int, int>>("absl + std::mutex");
	small_row<AbslCache<std::shared_mutex, int, int>>("absl + std::shared_mutex");
#endif
	if (check_only) std::puts("generation_cache_bench: every comparator returns the same results as Cache");
	return 0;
}
