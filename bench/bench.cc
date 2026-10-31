// Benchmark: generation_cache::Cache against the original (std::map + std::mutex)
// and against abseil flat_hash_map behind a mutex / shared_mutex (comparators).
// get() hit with string keys, 1 thread and 4 threads; ns per op, best of 5.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "generation_cache.hh"

static volatile size_t sink;

class OldCache {
	mutable std::mutex m_;
	std::map<std::string, std::string> e_;
public:
	std::optional<std::string> get(const std::string& k) const {
		std::lock_guard<std::mutex> l(m_);
		auto it = e_.find(k);
		if (it == e_.end()) return std::nullopt;
		return it->second;
	}
	void put(const std::string& k, std::string v) { std::lock_guard<std::mutex> l(m_); e_[k] = std::move(v); }
};

template <class M>
class AbslCache {
	mutable M m_;
	absl::flat_hash_map<std::string, std::string> e_;
public:
	std::optional<std::string> get(const std::string& k) const {
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
	void put(const std::string& k, std::string v) { std::unique_lock<M> l(m_); e_[k] = std::move(v); }
};

struct NewCache {
	generation_cache::Cache<std::string, std::string> c;
	std::optional<std::string> get(const std::string& k) const { return c.get(k); }
	void put(const std::string& k, std::string v) { c.try_insert(k, std::move(v), c.generation()); }
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

int main() {
	for (size_t n : {64u, 4096u, 100000u}) {
		std::vector<std::string> keys(n);
		for (size_t i = 0; i < n; ++i) keys[i] = "/api/v1/resource/" + std::to_string(i) + "?etag=abc";
		row<OldCache>("old std::map+mutex", n, keys);
		row<NewCache>("Cache (this library)", n, keys);
		row<AbslCache<std::mutex>>("mutex+absl flat_hash_map", n, keys);
		row<AbslCache<std::shared_mutex>>("shared_mutex+absl", n, keys);
	}
}
