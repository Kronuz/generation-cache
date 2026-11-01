// The reader-writer lock behind Cache (generation_cache::detail::rw_lock): readers share it,
// a writer is exclusive, a waiting writer holds new readers back, and nobody is starved.
// Run under ThreadSanitizer in CI; the plain a/b pair below is the data TSan watches.
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "generation_cache.hh"

using Lock = generation_cache::detail::rw_lock;
using namespace std::chrono_literals;

static void check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAILED: %s\n", what);
		std::abort();
	}
}

// Waits for a condition, giving up after `ms` milliseconds (a hang is a failure, not a stuck CI job).
template <typename F>
static bool wait_for(F&& f, int ms = 5000) {
	for (int i = 0; i < ms; ++i) {
		if (f()) return true;
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

// Two readers are inside at once.
static void readers_share() {
	Lock lock;
	std::atomic<bool> a_in{false}, b_in{false}, release{false};
	std::thread a([&] {
		std::shared_lock<Lock> l(lock);
		a_in = true;
		wait_for([&] { return b_in.load(); });
		release = true;
	});
	check(wait_for([&] { return a_in.load(); }), "reader A entered");
	std::thread b([&] {
		std::shared_lock<Lock> l(lock);
		b_in = true;
	});
	a.join();
	b.join();
	check(release.load() && b_in.load(), "a second reader entered while the first still held the lock");
	std::puts("rw_lock: readers share the lock OK");
}

// A writer excludes everybody, and sees all of the previous writers' and readers' effects. a and b
// are plain integers a writer keeps equal; a reader that sees them differ has seen a writer midway.
static void exclusion_stress() {
	Lock lock;
	long a = 0, b = 0;
	std::atomic<int> readers_in{0}, writers_in{0};
	std::atomic<bool> stop{false};
	std::atomic<long> reads{0}, writes{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) {
		ts.emplace_back([&] {
			while (!stop) {
				std::shared_lock<Lock> l(lock);
				readers_in.fetch_add(1);
				check(writers_in.load() == 0, "a reader and a writer were inside together");
				check(a == b, "a reader saw a half-written pair");
				readers_in.fetch_sub(1);
				reads.fetch_add(1);
			}
		});
	}
	for (int t = 0; t < 2; ++t) {
		ts.emplace_back([&] {
			while (!stop) {
				std::unique_lock<Lock> l(lock);
				check(writers_in.fetch_add(1) == 0, "two writers were inside together");
				check(readers_in.load() == 0, "a writer was inside with a reader");
				++a;
				std::this_thread::yield();
				++b;
				writers_in.fetch_sub(1);
				writes.fetch_add(1);
			}
		});
	}
	std::this_thread::sleep_for(1500ms);
	stop = true;
	for (auto& t : ts) t.join();
	check(reads > 0 && writes > 0, "readers and writers both made progress");
	check(a == b && a == writes.load(), "every write is accounted for");
	std::printf("rw_lock: exclusion under load OK (%ld reads, %ld writes)\n", reads.load(), writes.load());
}

// A writer waiting for a reader holds new readers back, and gets the lock when the reader leaves.
static void waiting_writer_blocks_new_readers() {
	Lock lock;
	std::atomic<bool> writer_in{false}, writer_done{false}, late_reader_in{false};
	std::unique_ptr<std::shared_lock<Lock>> first(new std::shared_lock<Lock>(lock));
	std::thread writer([&] {
		std::unique_lock<Lock> l(lock);
		writer_in = true;
		writer_done = true;
	});
	std::this_thread::sleep_for(100ms);  // the writer is now waiting for `first`
	std::thread late([&] {
		std::shared_lock<Lock> l(lock);
		late_reader_in = true;
		check(writer_done.load(), "a reader that arrived while a writer waited got in before the writer");
	});
	std::this_thread::sleep_for(100ms);
	check(!writer_in.load(), "the writer got in while a reader held the lock");
	check(!late_reader_in.load(), "a new reader got in past a waiting writer");
	first.reset();
	check(wait_for([&] { return writer_in.load(); }), "the writer got the lock once the reader left");
	writer.join();
	late.join();
	check(late_reader_in.load(), "the held-back reader got in after the writer");
	std::puts("rw_lock: a waiting writer holds new readers back OK");
}

// Readers that never pause cannot starve a writer, and a stream of writers cannot starve a reader.
// The check is progress, not rate: a turn can take milliseconds on a loaded two-core VM, and
// starvation is a side that never gets one.
static void nobody_starves() {
	{
		Lock lock;
		std::atomic<bool> stop{false};
		std::vector<std::thread> readers;
		for (int t = 0; t < 4; ++t)
			readers.emplace_back([&] { while (!stop) { std::shared_lock<Lock> l(lock); } });
		std::atomic<int> done{0};
		std::thread writer([&] {
			for (int i = 0; i < 100; ++i) { std::unique_lock<Lock> l(lock); done.fetch_add(1); }
		});
		bool ok = wait_for([&] { return done.load() == 100; }, 60000);
		stop = true;
		for (auto& t : readers) t.join();
		writer.join();
		check(ok, "a writer finished 100 turns among four busy readers");
	}
	{
		Lock lock;
		std::atomic<bool> stop{false};
		std::vector<std::thread> writers;
		for (int t = 0; t < 2; ++t)
			writers.emplace_back([&] { while (!stop) { std::unique_lock<Lock> l(lock); } });
		std::atomic<int> done{0};
		std::thread reader([&] {
			for (int i = 0; i < 100; ++i) { std::shared_lock<Lock> l(lock); done.fetch_add(1); }
		});
		bool ok = wait_for([&] { return done.load() == 100; }, 60000);
		stop = true;
		for (auto& t : writers) t.join();
		reader.join();
		check(ok, "a reader finished 100 turns among two busy writers");
	}
	std::puts("rw_lock: no starvation either way OK");
}

int main() {
	std::thread watchdog([] {
		std::this_thread::sleep_for(120s);
		std::fprintf(stderr, "FAILED: the lock test hung\n");
		std::abort();
	});
	watchdog.detach();
	readers_share();
	exclusion_stress();
	waiting_writer_blocks_new_readers();
	nobody_starves();
	std::puts("all rw_lock tests passed");
	return 0;
}
