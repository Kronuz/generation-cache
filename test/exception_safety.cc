// Strong exception guarantee of try_insert, including the table rehash.
//
// Oracle: a std::map<int, int> model, kept outside the cache. After every
// injected failure the cache must equal the model from before the call: the
// same size(), every model key found with its model value, every other key
// absent. After the failure the same call is retried with injection off and
// must succeed, so the cache is also still usable. Live-object counters
// (and ASan/LSan on the sanitizer jobs) catch leaks and double destruction.
//
// Failures are injected by element types whose copy/move throws on the Nth
// operation after arming, and by a replaced global operator new that throws
// std::bad_alloc on the Nth allocation after arming. N is swept across every
// operation of an insert sequence that crosses several rehashes (16 -> 32 ->
// 64 -> 128 slots), so each element move/copy and the table allocation of
// each rehash is the failing one in turn.
#undef NDEBUG
#include "../generation_cache.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

// A replaced operator new clashes with the static ASan/TSan runtimes (multiple
// definition at link time), so the bad_alloc runs happen only in unsanitized
// builds; the element copy/move runs cover the rehash under the sanitizers.
// -DGC_FORCE_ALLOC_INJECTION keeps them on for a shared-runtime sanitizer build.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define GC_SANITIZED 1
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#    define GC_SANITIZED 1
#  endif
#endif
#if !defined(GC_SANITIZED) || defined(GC_FORCE_ALLOC_INJECTION)
#  define GC_ALLOC_INJECTION 1
#endif

static long g_alloc = -1;  // armed: throw bad_alloc when it counts down to 0
#ifdef GC_ALLOC_INJECTION
void* operator new(std::size_t n) {
	if (g_alloc >= 0 && g_alloc-- == 0) throw std::bad_alloc();
	if (void* p = std::malloc(n ? n : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

static long g_op = -1;          // armed: an element copy/move throws when it counts down to 0
static bool g_moves_throw = false;  // every throwing-move element's move throws
static long g_live = 0;         // live Obj instances (leak / double-destroy check)
static long g_copies = 0;       // Obj copy constructions and copy assignments
static int g_failures = 0;

#define EXPECT(cond, ...) do { \
	if (!(cond)) { if (g_failures++ < 20) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } \
} while (0)

static void tick() { if (g_op >= 0 && g_op-- == 0) throw 42; }

// An element whose copy always ticks and whose move ticks too when the move is
// declared throwing. v = -1 marks a moved-from object, so a table entry that
// was moved out of and then lost shows up as a wrong value.
template <typename Tag, bool NothrowMove>
struct Obj {
	int v;
	Obj(int x = 0) : v(x) { ++g_live; }
	Obj(const Obj& o) : v(o.v) { ++g_copies; tick(); ++g_live; }
	Obj(Obj&& o) noexcept(NothrowMove) : v(o.v) {
		if constexpr (!NothrowMove) { tick(); if (g_moves_throw) throw 43; }
		o.v = -1;
		++g_live;
	}
	Obj& operator=(const Obj& o) { ++g_copies; tick(); v = o.v; return *this; }
	Obj& operator=(Obj&& o) noexcept(NothrowMove) {
		if constexpr (!NothrowMove) { tick(); if (g_moves_throw) throw 43; }
		v = o.v;
		o.v = -1;
		return *this;
	}
	~Obj() { --g_live; }
	bool operator==(const Obj& o) const { return v == o.v; }
	bool operator<(const Obj& o) const { return v < o.v; }
};
namespace std {
	template <typename Tag, bool N> struct hash<Obj<Tag, N>> {
		size_t operator()(const Obj<Tag, N>& o) const { return std::hash<int>{}(o.v); }
	};
}
struct KTag {};
struct VTag {};
using ThrowKey = Obj<KTag, false>;  // copy and move can throw
using ThrowVal = Obj<VTag, false>;
using SafeKey = Obj<KTag, true>;    // copy can throw, move cannot
using SafeVal = Obj<VTag, true>;

// A Key with only operator<: stored in the std::map adapter, not the hash table.
struct OrderedKey {
	int v;
	explicit OrderedKey(int x = 0) : v(x) {}
	bool operator<(const OrderedKey& o) const { return v < o.v; }
};

static int val(int x) { return x; }
template <typename T, bool N> static int val(const Obj<T, N>& o) { return o.v; }

template <typename T> static T make(int x) {
	if constexpr (std::is_same_v<T, std::string>) return std::string(40 + x, 'x');  // beyond SSO
	else return T(x);
}
static int valof(const std::string& s) { return static_cast<int>(s.size()) - 40; }
template <typename T> static int back(const T& t) {
	if constexpr (std::is_same_v<T, std::string>) return valof(t);
	else return val(t);
}

static void disarm() { g_op = -1; g_alloc = -1; }

// The cache must equal the model, exactly.
template <typename K, typename C>
static void check(const C& c, const std::map<int, int>& model, const char* name, long n, const char* step) {
	disarm();
	EXPECT(c.size() == model.size(), "%s N=%ld %s: size %zu, model %zu", name, n, step, c.size(), model.size());
	for (int k = 1; k <= 45; ++k) {
		auto got = c.get(make<K>(k));
		auto it = model.find(k);
		if (it == model.end()) EXPECT(!got.has_value(), "%s N=%ld %s: key %d present but not inserted", name, n, step, k);
		else EXPECT(got.has_value() && back(*got) == it->second, "%s N=%ld %s: key %d lost or wrong", name, n, step, k);
	}
}

enum class Inject { ops, allocs };

// Insert 1..40 (and, with `replace`, overwrite key i-1 first at every step, so
// the rehash that an insert at size 8, 16 or 32 triggers also runs for an
// overwrite). Each call first runs with injection armed at N; if it throws,
// the cache is checked against the model and the call is retried disarmed.
template <typename K, typename V>
static int run(const char* name, Inject mode, long maxN, bool replace) {
	int injected = 0;
	for (long N = 0; N <= maxN; ++N) {
		{
			generation_cache::Cache<K, V> c;
			std::map<int, int> model;
			auto attempt = [&](int key, int value) {
				for (int pass = 0; pass < 2; ++pass) {
					bool armed = pass == 0;
					K k = make<K>(key);
					V v = make<V>(value);
					bool threw = false, committed = false;
					try {
						(mode == Inject::ops ? g_op : g_alloc) = armed ? N : -1;
						committed = c.try_insert(k, std::move(v), c.generation());
					} catch (const std::bad_alloc&) { threw = true; } catch (int) { threw = true; }
					disarm();
					if (!threw) {
						EXPECT(committed, "%s N=%ld: fresh-generation insert refused", name, N);
						model[key] = value;
						return;
					}
					++injected;
					check<K>(c, model, name, N, "after failed insert");
				}
				EXPECT(false, "%s N=%ld: retry of key %d failed", name, N, key);
			};
			for (int i = 1; i <= 40; ++i) {
				if (replace && i > 1) attempt(i - 1, 1000 + i);
				attempt(i, i * 10);
				check<K>(c, model, name, N, "after insert");
			}
		}
		EXPECT(g_live == 0, "%s N=%ld: %ld live objects after destruction (leak or double destroy)", name, N, g_live);
		g_live = 0;
	}
	std::printf("%s: %d injected failures\n", name, injected);
	return injected;
}

// Moves always throw but copies work: the rehash must pick the copy path, so
// every insert and overwrite succeeds with no injection at all.
static void moves_always_throw() {
	g_moves_throw = true;
	{
		generation_cache::Cache<ThrowKey, int> c;
		try {
			for (int i = 1; i <= 100; ++i) EXPECT(c.try_insert(ThrowKey(i), i * 10, c.generation()), "insert %d", i);
		} catch (int) { EXPECT(false, "an always-throwing move escaped try_insert (rehash moved instead of copied)"); }
		disarm();
		EXPECT(c.size() == 100, "size %zu", c.size());
		for (int i = 1; i <= 100; ++i) {
			auto got = c.get(ThrowKey(i));
			EXPECT(got && *got == i * 10, "key %d lost with always-throwing moves", i);
		}
	}
	g_moves_throw = false;
	EXPECT(g_live == 0, "moves_always_throw: %ld live objects", g_live);
	g_live = 0;
}

// A Key and Value with a nothrow move must rehash by moving: the only copy
// during the 9th insert (which rehashes 16 -> 32 slots) is the insert's own key
// copy, so a throwing copy can never fail a rehash.
static void nothrow_move_never_copies_on_rehash() {
	generation_cache::Cache<SafeKey, SafeVal> c;
	for (int i = 1; i <= 8; ++i) EXPECT(c.try_insert(SafeKey(i), SafeVal(i), c.generation()), "insert %d", i);
	SafeKey k(9);
	SafeVal v(9);
	g_copies = 0;
	EXPECT(c.try_insert(k, std::move(v), c.generation()), "insert 9");
	EXPECT(g_copies == 1, "rehash of nothrow-move elements made %ld copies (want 1, the insert's own key)", g_copies);
}

// Move-only types: the strong guarantee is not available when their move can
// throw (as for std::vector), but they must still compile and work.
struct MoveOnlyNoexcept {
	std::unique_ptr<int> p;
	explicit MoveOnlyNoexcept(int x = 0) : p(new int(x)) {}
};
struct MoveOnlyThrowing {
	int v;
	explicit MoveOnlyThrowing(int x = 0) : v(x) {}
	MoveOnlyThrowing(MoveOnlyThrowing&& o) noexcept(false) : v(o.v) {}
	MoveOnlyThrowing& operator=(MoveOnlyThrowing&& o) noexcept(false) { v = o.v; return *this; }
};
static void move_only_types() {
	{
		generation_cache::Cache<int, std::unique_ptr<int>> c;
		for (int i = 1; i <= 40; ++i) c.try_insert(i, std::make_unique<int>(i), c.generation());
		EXPECT(c.size() == 40, "unique_ptr size");
	}
	{
		generation_cache::Cache<int, MoveOnlyThrowing> c;
		for (int i = 1; i <= 40; ++i) c.try_insert(i, MoveOnlyThrowing(i), c.generation());
		EXPECT(c.size() == 40, "move-only throwing size");
	}
}

int main() {
	static_assert(std::is_same_v<generation_cache::detail::entries_for<ThrowKey, int>::type,
		generation_cache::detail::hash_entries<ThrowKey, int>>, "ThrowKey must use the hash table");
	static_assert(std::is_same_v<generation_cache::detail::entries_for<OrderedKey, int>::type,
		generation_cache::detail::ordered_entries<OrderedKey, int>>, "OrderedKey must use std::map");

	int injected = 0;
	// Element copy/move throws on the Nth operation (N past ~140 never fires).
	injected += run<ThrowKey, int>("hash, Key copy/move throws", Inject::ops, 150, true);
	injected += run<int, ThrowVal>("hash, Value copy/move throws", Inject::ops, 150, false);
	injected += run<ThrowKey, ThrowVal>("hash, Key and Value throw", Inject::ops, 300, false);
	injected += run<SafeKey, SafeVal>("hash, nothrow move, throwing copy", Inject::ops, 150, true);
#ifdef GC_ALLOC_INJECTION
	// operator new throws bad_alloc on the Nth allocation.
	injected += run<ThrowKey, int>("hash, bad_alloc (Key)", Inject::allocs, 4, true);
	injected += run<std::string, std::string>("hash, bad_alloc (strings)", Inject::allocs, 6, true);
#endif
	// The std::map adapter, for a Key with only operator<.
	injected += run<OrderedKey, ThrowVal>("ordered, Value copy/move throws", Inject::ops, 10, false);
#ifdef GC_ALLOC_INJECTION
	injected += run<OrderedKey, int>("ordered, bad_alloc", Inject::allocs, 4, true);
#else
	std::puts("bad_alloc runs skipped (sanitizer build; see the comment at the top)");
#endif

	moves_always_throw();
	nothrow_move_never_copies_on_rehash();
	move_only_types();

	EXPECT(injected > 0, "no failure was injected");
	if (g_failures) { std::printf("%d failures\n", g_failures); return 1; }
	std::puts("all exception-safety tests passed");
}
