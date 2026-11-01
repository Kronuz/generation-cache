// The cache against a reference model, over long random operation sequences.
//
// The model is a std::unordered_map plus the documented rule: try_insert commits (and overwrites)
// only when its generation equals the current one, clear() drops every entry and advances the
// generation by one, get and get_with_generation read the map and the generation. Every result of
// every operation is compared with the model's, and after each run every key of the universe is
// compared once more. The runs use keys whose hashes are good, identical, few, high-bit-only and
// strided (so that probe sequences overflow groups and wrap), strings of every length around the
// small-string size, and universes that grow the table from nothing through many doublings.
// The hash table is also driven directly, checking the capacity rules at every step.
#undef NDEBUG
#include "../generation_cache.hh"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// A key whose std::hash is chosen by `Kind`, to steer the table's collisions.
template <int Kind>
struct HKey {
	std::uint64_t v;
	bool operator==(const HKey& o) const { return v == o.v; }
};
namespace std {
	template <int Kind>
	struct hash<HKey<Kind>> {
		size_t operator()(const HKey<Kind>& k) const noexcept {
			switch (Kind) {
				case 0: return static_cast<size_t>(k.v);                   // identity
				case 1: return 0;                                         // every key collides
				case 2: return static_cast<size_t>(k.v & 0xFF);            // 256 distinct hashes
				case 3: return static_cast<size_t>(k.v << 40);             // the high bits only
				case 4: return static_cast<size_t>(k.v * 4096);            // a power-of-two stride
				default: return static_cast<size_t>(k.v * 0x9E3779B97F4A7C15ULL);  // a well mixed hash
			}
		}
	};
}

// A key with a custom hash and equality.
struct Point {
	int x, y;
	bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};
namespace std {
	template <> struct hash<Point> {
		size_t operator()(const Point& p) const noexcept { return static_cast<size_t>(p.x) * 31u + static_cast<size_t>(p.y); }
	};
}

static std::uint64_t rng_state;
static std::uint64_t next_random() {
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return rng_state >> 24;
}

template <typename Key, typename Value>
class Model {
public:
	std::uint64_t generation = 0;
	std::unordered_map<Key, Value> entries;
	const Value* get(const Key& k) const {
		auto it = entries.find(k);
		return it == entries.end() ? nullptr : &it->second;
	}
	bool try_insert(const Key& k, Value v, std::uint64_t at) {
		if (at != generation) return false;
		entries[k] = std::move(v);
		return true;
	}
	void clear() { entries.clear(); ++generation; }
};

// Distinct keys of every length from 0 to 46 bytes (the empty key is key 0).
static std::string make_string_key(std::uint64_t i) {
	if (i == 0) return std::string();
	return std::to_string(i) + std::string(i % 41, static_cast<char>('a' + i % 26));
}
static std::string make_string_value(std::uint64_t x) { return std::string(x % 40, 'v') + std::to_string(x); }

struct Counters {
	long ops = 0, hits = 0, misses = 0, committed = 0, stale = 0, clears = 0;
};
static Counters total;

// One run: `steps` random operations over `universe` keys, then a full comparison.
template <typename Key, typename Value, typename MakeKey, typename MakeValue>
static void run(const char* name, std::size_t universe, long steps, unsigned seed, long clear_every, MakeKey make_key, MakeValue make_value) {
	rng_state = seed * 7919u + universe;
	generation_cache::Cache<Key, Value> cache;
	Model<Key, Value> model;
	std::vector<std::uint64_t> past{0};  // generations seen, to feed stale inserts
	auto same = [&](const Key& k) {
		const auto got = cache.get(k);
		const Value* want = model.get(k);
		assert(got.has_value() == (want != nullptr));
		if (want) assert(*got == *want);
	};
	for (long step = 0; step < steps; ++step) {
		const Key k = make_key(next_random() % universe);
		const unsigned op = static_cast<unsigned>(next_random() % 100);
		++total.ops;
		if (clear_every > 0 && next_random() % static_cast<std::uint64_t>(clear_every) == 0) {
			cache.clear();
			model.clear();
			past.push_back(model.generation);
			++total.clears;
			assert(cache.size() == 0);
		} else if (op < 40) {
			same(k);
		} else if (op < 50) {
			const auto got = cache.get_with_generation(k);
			const Value* want = model.get(k);
			assert(got.generation == model.generation);
			assert(got.value.has_value() == (want != nullptr));
			if (want) assert(*got.value == *want);
		} else if (op < 85) {  // an insert at the current generation: always committed
			Value v = make_value(next_random());
			assert(cache.try_insert(k, v, model.generation));
			assert(model.try_insert(k, v, model.generation));
			++total.committed;
		} else if (op < 92) {  // an insert at some past or the current generation
			const std::uint64_t at = past[next_random() % past.size()];
			Value v = make_value(next_random());
			const bool a = cache.try_insert(k, v, at), b = model.try_insert(k, v, at);
			assert(a == b);
			if (!a) ++total.stale;
			if (at != model.generation) assert(!a);
		} else {
			assert(cache.size() == model.entries.size());
			assert(cache.generation() == model.generation);
		}
		if (step % 4096 == 4095) {
			for (std::size_t i = 0; i < universe; ++i) same(make_key(i));
			assert(cache.size() == model.entries.size());
		}
	}
	for (std::size_t i = 0; i < universe; ++i) same(make_key(i));
	assert(cache.size() == model.entries.size() && cache.generation() == model.generation);
	for (const auto& kv : model.entries) {
		const auto got = cache.get(kv.first);
		assert(got && *got == kv.second);
	}
	(void)name;
}

// Insert every key of the universe in order, so the table passes through every capacity, and check
// each insert, then overwrite them all.
template <typename Key, typename MakeKey>
static void fill(std::size_t universe, MakeKey make_key) {
	generation_cache::Cache<Key, std::uint64_t> cache;
	for (std::size_t i = 0; i < universe; ++i) {
		assert(cache.try_insert(make_key(i), i, cache.generation()));
		assert(cache.size() == i + 1);
		if (i % 97 == 0 || i < 40) {
			for (std::size_t j = 0; j <= i; j += (i < 200 ? 1 : 13)) assert(cache.get(make_key(j)).value() == j);
		}
		assert(!cache.get(make_key(universe + i)).has_value());
	}
	for (std::size_t i = 0; i < universe; ++i) assert(cache.get(make_key(i)).value() == i);
	for (std::size_t i = 0; i < universe; ++i) assert(cache.try_insert(make_key(i), i + 1, cache.generation()));
	assert(cache.size() == universe);
	for (std::size_t i = 0; i < universe; ++i) assert(cache.get(make_key(i)).value() == i + 1);
}

// The table on its own: the capacity is a power of two that never exceeds what the load rule
// allows, doubles only when an insert finds no room, and a clear starts over.
template <typename Key, typename MakeKey>
static void table(std::size_t universe, MakeKey make_key) {
	using Table = generation_cache::detail::hash_entries<Key, std::size_t>;
	Table t;
	assert(t.capacity() == 0 && t.size() == 0 && t.find(make_key(0)) == nullptr);
	for (int round = 0; round < 3; ++round) {
		std::size_t previous = 0;
		for (std::size_t i = 0; i < universe; ++i) {
			const std::size_t before = t.capacity();
			t.insert_or_assign(make_key(i), i + round);
			const std::size_t c = t.capacity();
			assert(c >= 4 && (c & (c - 1)) == 0);
			assert(t.size() == i + 1);
			assert(t.size() <= c - (c / 8 ? c / 8 : 1));  // an empty slot always exists
			assert(c == before || c == (before ? before * 2 : 4));
			if (c != before) assert(before == 0 || i + 1 > before - (before / 8 ? before / 8 : 1));  // grew only when full
			assert(c >= previous);
			previous = c;
			t.insert_or_assign(make_key(i), i + round);  // an overwrite never grows or adds
			assert(t.capacity() == c && t.size() == i + 1);
			assert(*t.find(make_key(i)) == i + round);
		}
		for (std::size_t i = 0; i < universe; ++i) assert(*t.find(make_key(i)) == i + round);
		t.clear();
		assert(t.capacity() == 0 && t.size() == 0 && t.find(make_key(0)) == nullptr);
	}
}

// How many groups a lookup examines, on average and at worst, with keys whose std::hash is poor.
// The seed and the multiplication spread them: the identity hash on sequential, strided and
// high-bit keys must stay within a small bound.
template <typename Key, typename MakeKey>
static void probe_lengths(const char* name, std::size_t universe, double max_mean, std::size_t max_worst, MakeKey make_key) {
	generation_cache::detail::hash_entries<Key, int> t;
	for (std::size_t i = 0; i < universe; ++i) t.insert_or_assign(make_key(i), 1);
	std::size_t sum = 0, worst = 0;
	for (std::size_t i = 0; i < universe; ++i) {
		const std::size_t p = t.probes(make_key(i));
		assert(p >= 1);
		sum += p;
		if (p > worst) worst = p;
	}
	const double mean = static_cast<double>(sum) / static_cast<double>(universe);
	std::printf("  probes %-28s n=%-7zu mean %.3f worst %zu (capacity %zu)\n", name, universe, mean, worst, t.capacity());
	assert(mean <= max_mean && worst <= max_worst);
}

int main() {
	auto str = [](std::uint64_t i) { return make_string_key(i); };
	auto integer = [](std::uint64_t i) { return static_cast<int>(i); };
	auto point = [](std::uint64_t i) { return Point{static_cast<int>(i % 97), static_cast<int>(i / 97)}; };
	auto sval = [](std::uint64_t x) { return make_string_value(x); };
	auto ival = [](std::uint64_t x) { return static_cast<int>(x % 1000); };

	for (unsigned seed = 1; seed <= 3; ++seed) {
		run<std::string, std::string>("string/string 40", 40, 60000, seed, 4000, str, sval);
		run<std::string, std::string>("string/string 3000", 3000, 200000, seed, 40000, str, sval);
		run<std::string, int>("string/int 30000", 30000, 250000, seed, 0, str, ival);
		run<int, std::string>("int/string 700", 700, 100000, seed, 6000, integer, sval);
		run<int, int>("int/int 200000", 200000, 300000, seed, 0, integer, ival);
		run<Point, std::string>("point/string 5000", 5000, 100000, seed, 20000, point, sval);
		run<HKey<0>, int>("identity 20000", 20000, 150000, seed, 0, [](std::uint64_t i) { return HKey<0>{i}; }, ival);
		run<HKey<1>, std::string>("constant 400", 400, 60000, seed, 5000, [](std::uint64_t i) { return HKey<1>{i}; }, sval);
		run<HKey<2>, int>("256 hashes 3000", 3000, 80000, seed, 0, [](std::uint64_t i) { return HKey<2>{i}; }, ival);
		run<HKey<3>, std::string>("high bits 6000", 6000, 80000, seed, 30000, [](std::uint64_t i) { return HKey<3>{i}; }, sval);
		run<HKey<4>, int>("stride 4096 x 20000", 20000, 100000, seed, 0, [](std::uint64_t i) { return HKey<4>{i}; }, ival);
		run<HKey<5>, int>("mixed 5000", 5000, 100000, seed, 12000, [](std::uint64_t i) { return HKey<5>{i}; }, ival);
	}
	std::printf("model: %ld operations, %ld inserts committed, %ld refused as stale, %ld clears\n",
		total.ops, total.committed, total.stale, total.clears);

	fill<std::string>(70000, str);
	fill<int>(70000, integer);
	fill<HKey<1>>(700, [](std::uint64_t i) { return HKey<1>{i}; });
	fill<HKey<2>>(5000, [](std::uint64_t i) { return HKey<2>{i}; });
	fill<HKey<4>>(40000, [](std::uint64_t i) { return HKey<4>{i}; });

	table<std::string>(5000, str);
	table<int>(5000, integer);
	table<HKey<1>>(300, [](std::uint64_t i) { return HKey<1>{i}; });
	table<HKey<3>>(3000, [](std::uint64_t i) { return HKey<3>{i}; });

	// A well spread hash needs 1.04 groups per lookup (1.1 with groups of 8) at this load; the bounds
	// leave room for the seed, which changes in every run. A weak mix needs far more.
	probe_lengths<int>("sequential ints", 100000, 1.25, 40, integer);
	probe_lengths<HKey<0>>("identity hash, sequential", 100000, 1.25, 40, [](std::uint64_t i) { return HKey<0>{i}; });
	probe_lengths<HKey<3>>("identity of k << 40", 50000, 1.25, 40, [](std::uint64_t i) { return HKey<3>{i}; });
	probe_lengths<HKey<4>>("identity of k * 4096", 50000, 1.25, 40, [](std::uint64_t i) { return HKey<4>{i}; });
	probe_lengths<HKey<5>>("identity of k * golden", 100000, 1.25, 40, [](std::uint64_t i) { return HKey<5>{i}; });
	probe_lengths<std::string>("strings", 100000, 1.25, 40, str);
	std::puts("all model tests passed");
	return 0;
}
