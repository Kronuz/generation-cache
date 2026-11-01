// The group test of the table's control bytes: `group::equal` selects exactly the slots whose
// byte equals a value, lowest first, for every byte value and every position, on random
// contents, and with the empty and padding bytes the table uses. Built for the platform's group
// (SSE2, NEON or the 64-bit word) and again with GENERATION_CACHE_NO_SIMD, which is the word.
#undef NDEBUG
#include "../generation_cache.hh"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using generation_cache::detail::ctrl_empty;
using generation_cache::detail::ctrl_padding;
using generation_cache::detail::group;

static constexpr std::size_t W = group::width;

// The slots of `bytes` equal to `value`, by a byte loop.
static std::vector<unsigned> expected(const std::uint8_t* bytes, unsigned value) {
	std::vector<unsigned> slots;
	for (std::size_t i = 0; i < W; ++i) {
		if (bytes[i] == value) slots.push_back(static_cast<unsigned>(i));
	}
	return slots;
}

static void check(const std::uint8_t* bytes, unsigned value) {
	const std::vector<unsigned> want = expected(bytes, value);
	auto mask = group(bytes).equal(static_cast<std::uint8_t>(value));
	assert(static_cast<bool>(mask) == !want.empty());
	std::vector<unsigned> got;
	for (; mask; mask.clear_first()) {
		got.push_back(mask.first());
		assert(got.size() <= W);
	}
	assert(got == want);
}

int main() {
	std::printf("group width %zu\n", W);
	std::uint8_t bytes[64];

	// One byte of every value at every position among empty bytes, tested against every value.
	for (std::size_t pos = 0; pos < W; ++pos) {
		for (unsigned b = 0; b < 256; ++b) {
			for (std::size_t i = 0; i < W; ++i) bytes[i] = ctrl_empty;
			bytes[pos] = static_cast<std::uint8_t>(b);
			for (unsigned v = 0; v < 256; ++v) check(bytes, v);
		}
	}

	// Padding after the last slot of a small table is neither a tag nor empty.
	for (std::size_t slots = 1; slots < W; ++slots) {
		for (std::size_t i = 0; i < W; ++i) bytes[i] = i < slots ? 0x2A : ctrl_padding;
		for (unsigned v = 0; v < 128; ++v) check(bytes, v);
		check(bytes, ctrl_empty);
		assert(!group(bytes).equal(ctrl_empty));
		assert(group(bytes).equal(0x2A).first() == 0);
	}

	// Random contents drawn from few values, so that several slots are equal.
	std::uint64_t r = 12345;
	auto next = [&] { r = r * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<unsigned>(r >> 33); };
	for (int trial = 0; trial < 200000; ++trial) {
		const unsigned alphabet = 1 + next() % 6;
		unsigned pool[6];
		for (unsigned& p : pool) p = next() % 3 == 0 ? (next() % 2 ? ctrl_empty : ctrl_padding) : next() % 128;
		for (std::size_t i = 0; i < W; ++i) bytes[i] = static_cast<std::uint8_t>(pool[next() % alphabet]);
		for (unsigned k = 0; k < alphabet; ++k) check(bytes, pool[k]);
		check(bytes, next() % 256);
	}

	// A group read at any offset of a larger buffer sees the bytes there, not the aligned ones.
	std::uint8_t buffer[96];
	for (unsigned i = 0; i < sizeof buffer; ++i) buffer[i] = static_cast<std::uint8_t>(i % 5 == 0 ? 7 : ctrl_empty);
	for (std::size_t offset = 0; offset + W <= sizeof buffer; ++offset) {
		check(buffer + offset, 7);
		check(buffer + offset, ctrl_empty);
	}
	std::puts("all group tests passed");
	return 0;
}
