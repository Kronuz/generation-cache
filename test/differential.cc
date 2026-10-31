// Differential test: the cache against a frozen copy of the original
// implementation (one std::map under one std::mutex), driven by the same random
// operation sequence. Also checks the container choice: a hashable Key gets a
// hash table, a Key with only operator< still compiles and uses std::map.
#undef NDEBUG
#include "../generation_cache.hh"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <new>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

template <typename Key, typename Value>
class FrozenCache {
public:
	using Generation = std::uint64_t;
	struct Lookup { std::optional<Value> value; Generation generation = 0; };
	Generation generation() const { std::lock_guard<std::mutex> l(mutex_); return generation_; }
	std::optional<Value> get(const Key& key) const {
		std::lock_guard<std::mutex> l(mutex_);
		auto it = entries_.find(key);
		if (it == entries_.end()) return std::nullopt;
		return it->second;
	}
	Lookup get_with_generation(const Key& key) const {
		std::lock_guard<std::mutex> l(mutex_);
		Lookup r; r.generation = generation_;
		auto it = entries_.find(key);
		if (it != entries_.end()) r.value = it->second;
		return r;
	}
	bool try_insert(const Key& key, Value value, Generation at) {
		std::lock_guard<std::mutex> l(mutex_);
		if (generation_ != at) return false;
		entries_[key] = std::move(value);
		return true;
	}
	void clear() { std::lock_guard<std::mutex> l(mutex_); entries_.clear(); ++generation_; }
	std::size_t size() const { std::lock_guard<std::mutex> l(mutex_); return entries_.size(); }
private:
	mutable std::mutex mutex_;
	std::map<Key, Value> entries_;
	Generation generation_ = 0;
};

// Only operator< (no hash, no ==).
struct OrderedKey {
	int v;
	bool operator<(const OrderedKey& o) const { return v < o.v; }
};

static_assert(generation_cache::detail::is_hashable<std::string>::value);
static_assert(generation_cache::detail::is_hashable<int>::value);
static_assert(!generation_cache::detail::is_hashable<OrderedKey>::value);
static_assert(std::is_same_v<generation_cache::detail::entries_for<OrderedKey, int>::type, generation_cache::detail::ordered_entries<OrderedKey, int>>);
static_assert(std::is_same_v<generation_cache::detail::entries_for<int, int>::type, generation_cache::detail::hash_entries<int, int>>);

template <typename Key, typename MakeKey>
static void run(MakeKey make_key, unsigned seed) {
	generation_cache::Cache<Key, std::string> a;
	FrozenCache<Key, std::string> b;
	std::uint64_t r = seed;
	auto next = [&] { r = r * 6364136223846793005ULL + 1442695040888963407ULL; return r >> 33; };
	std::vector<std::uint64_t> stale_gens{0};  // old generations, to exercise rejected inserts
	for (int i = 0; i < 200000; ++i) {
		Key k = make_key(next() % 64);
		switch (next() % 8) {
			case 0: case 1: {
				auto ga = a.generation(), gb = b.generation();
				assert(ga == gb);
				std::string v = "v" + std::to_string(next() % 1000);
				bool use_stale = next() % 4 == 0;
				std::uint64_t g = use_stale ? stale_gens[next() % stale_gens.size()] : ga;
				assert(a.try_insert(k, v, g) == b.try_insert(k, v, g));
				break;
			}
			case 2: case 3: case 4: assert(a.get(k) == b.get(k)); break;
			case 5: {
				auto la = a.get_with_generation(k);
				auto lb = b.get_with_generation(k);
				assert(la.value == lb.value && la.generation == lb.generation);
				break;
			}
			case 6: assert(a.size() == b.size()); break;
			case 7:
				if (next() % 16 == 0) { a.clear(); b.clear(); stale_gens.push_back(a.generation()); }
				break;
		}
	}
	assert(a.size() == b.size() && a.generation() == b.generation());
}

// A Key whose copy can be made to throw; every key hashes alike so a failed
// insert's slot is exactly where a later probe for another key lands (CR1).
struct ThrowKey {
	int v;
	static inline bool throw_next = false;
	ThrowKey(int x) : v(x) {}
	ThrowKey(const ThrowKey& o) : v(o.v) { if (throw_next) { throw_next = false; throw std::bad_alloc(); } }
	ThrowKey(ThrowKey&&) = default;
	ThrowKey& operator=(const ThrowKey&) = default;
	ThrowKey& operator=(ThrowKey&&) = default;
	bool operator==(const ThrowKey& o) const { return v == o.v; }
};
template <> struct std::hash<ThrowKey> { size_t operator()(const ThrowKey&) const { return 7; } };

int main() {
	// CR1: a throwing Key copy leaves the cache unchanged and usable.
	{
		generation_cache::Cache<ThrowKey, std::string> c;
		assert(c.try_insert(ThrowKey(1), "a", c.generation()));
		ThrowKey::throw_next = true;
		bool threw = false;
		try { c.try_insert(ThrowKey(2), "b", c.generation()); } catch (const std::bad_alloc&) { threw = true; }
		assert(threw && c.size() == 1);
		assert(!c.get(ThrowKey(2)).has_value());
		assert(!c.get(ThrowKey(3)).has_value());
		assert(c.get(ThrowKey(1)).value() == "a");
		assert(c.try_insert(ThrowKey(2), "b", c.generation()));
		assert(c.size() == 2 && c.get(ThrowKey(2)).value() == "b");
	}

	for (unsigned seed = 1; seed <= 5; ++seed) {
		run<std::string>([](std::uint64_t n) { return n ? "key-" + std::to_string(n) : std::string(); }, seed);
		run<int>([](std::uint64_t n) { return int(n); }, seed);
		run<OrderedKey>([](std::uint64_t n) { return OrderedKey{int(n)}; }, seed);
	}

	// Growth: many keys, with clear() in the middle (table restarts small).
	{
		generation_cache::Cache<std::string, int> c;
		for (int round = 0; round < 3; ++round) {
			for (int i = 0; i < 20000; ++i) assert(c.try_insert("k" + std::to_string(i), i + round, c.generation()));
			assert(c.size() == 20000);
			for (int i = 0; i < 20000; ++i) assert(c.get("k" + std::to_string(i)).value() == i + round);
			assert(!c.get("nope").has_value());
			c.clear();
			assert(c.size() == 0 && !c.get("k1").has_value());
		}
	}

	// Many concurrent readers over a shared lock with a writer: values are
	// always one of the valid strings, size never exceeds the key space.
	generation_cache::Cache<int, std::string> c;
	std::atomic<bool> stop{false};
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) ts.emplace_back([&, t] {
		while (!stop) {
			auto l = c.get_with_generation(t);
			if (!l.value) c.try_insert(t, "x" + std::to_string(t), l.generation);
			else assert(*l.value == "x" + std::to_string(t));
			assert(c.size() <= 4);
		}
	});
	for (int i = 0; i < 5000; ++i) c.clear();
	stop = true;
	for (auto& t : ts) t.join();
	std::puts("generation-cache differential tests passed");
}
