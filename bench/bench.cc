// Benchmark: generation_cache::Cache against the original (std::map + std::mutex)
// and against abseil flat_hash_map behind a mutex / shared_mutex (comparators).
// get() hit with string keys, 1 thread and 4 threads, at three sizes; then the costs of
// a small cache: construct + destroy, and get hit and miss with 64 int keys. ns per op, best of 5.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "generation_cache.hh"

static volatile size_t sink;

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
class OldCache {
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

template <class K, class V>
struct NewCache {
	generation_cache::Cache<K, V> c;
	std::optional<V> get(const K& k) const { return c.get(k); }
	void put(const K& k, V v) { c.try_insert(k, std::move(v), c.generation()); }
	std::size_t size() const { return c.size(); }
};

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
	double t1 = 1e18, t4 = 1e18;
	for (int i = 0; i < 5; ++i) {
		t1 = std::min(t1, mt(c, keys, 1, 500000));
		t4 = std::min(t4, mt(c, keys, 4, 300000));
	}
	std::printf("  n=%-7zu %-26s 1 thread %7.1f ns   4 threads %7.1f ns/op\n", n, name, t1, t4);
}

using Str = std::string;

// A small cache: what building one costs, and a get that is over almost as soon as it starts.
template <class C>
static void small_row(const char* name) {
	double ctor = best_ns([] { for (int i = 0; i < 20000; ++i) { C c; sink = c.size(); } }, 20000);
	C c;
	for (int i = 0; i < 64; ++i) c.put(i, i);
	double hit = best_ns([&] { size_t s = 0; for (int r = 0; r < 4000; ++r) for (int i = 0; i < 64; ++i) s += c.get(i).value_or(0); sink = s; }, 64.0 * 4000);
	double miss = best_ns([&] { size_t s = 0; for (int r = 0; r < 4000; ++r) for (int i = 64; i < 128; ++i) s += c.get(i).has_value(); sink = s; }, 64.0 * 4000);
	std::printf("  %-26s construct+destroy %6.1f ns   get hit %6.2f ns   get miss %6.2f ns\n", name, ctor, hit, miss);
}

// usage: generation_cache_bench [n ...] [small]
// No arguments: string keys at n = 64, 4096 and 100000, then the small-cache rows.
// Sizes alone run only those string rows; `small` adds the small-cache rows.
int main(int argc, char** argv) {
	std::vector<size_t> sizes;
	bool small = argc < 2;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "small") small = true;
		else sizes.push_back(std::stoul(argv[i]));
	}
	if (argc < 2) sizes = {64, 4096, 100000};
	for (size_t n : sizes) {
		std::vector<std::string> keys(n);
		for (size_t i = 0; i < n; ++i) keys[i] = "/api/v1/resource/" + std::to_string(i) + "?etag=abc";
		row<OldCache<Str, Str>>("old std::map+mutex", n, keys);
		row<NewCache<Str, Str>>("Cache (this library)", n, keys);
		row<AbslCache<std::mutex, Str, Str>>("mutex+absl flat_hash_map", n, keys);
		row<AbslCache<std::shared_mutex, Str, Str>>("shared_mutex+absl", n, keys);
	}
	if (!small) return 0;
	std::printf("small cache, int keys, 1 thread\n");
	small_row<OldCache<int, int>>("old std::map+mutex");
	small_row<NewCache<int, int>>("Cache (this library)");
	small_row<AbslCache<std::mutex, int, int>>("mutex+absl flat_hash_map");
	small_row<AbslCache<std::shared_mutex, int, int>>("shared_mutex+absl");
	return 0;
}
