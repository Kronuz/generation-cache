# Architecture

The internals of `generation_cache`: one mutex guarding a map and a counter.
For usage see `README.md`; for the repo map and invariants see `AGENTS.md`.

## Shape

A single header, nothing else:

```
  generation_cache.hh   the templated Cache<Key, Value> class
```

No compiled translation unit, no external dependency. `generation_cache.hh`
includes only standard headers: `<atomic>`, `<cassert>`, `<chrono>`, `<cstddef>`,
`<cstdint>`, `<cstring>`, `<functional>`, `<map>`, `<mutex>`, `<new>`,
`<optional>`, `<random>`, `<shared_mutex>` (for `std::shared_lock`), `<thread>`,
`<type_traits>`, and `<utility>`, plus `<emmintrin.h>` (SSE2) or `<arm_neon.h>`
(NEON) where the platform has them.

## The problem this solves

A cache keyed by `Key` producing a `Value` usually has to answer: when is an
entry no longer good? Two common answers are "after N seconds" (TTL) and
"when something more recently used needs the slot" (LRU). Both rank entries
individually.

The motivating case here doesn't fit either model. Picture a server with one
piece of mutable state (a version counter, say) that changes in discrete
steps: an accepted write, a new commit. Many readers derive a `Value` (e.g.
a diff from some `Key`, a base version, to the current state) from that
state. Every `Value` computed before step N is valid until step N+1 arrives,
and then EVERY one of them is wrong at once, regardless of how recently or
often each was used. There is no partial staleness and no useful recency
ordering. An entry is either exactly current or completely wrong.

So the right invalidation unit isn't the entry, it's the whole cache, and the
right trigger isn't time or access pattern, it's "the thing I was computed
from changed." `clear()` is that trigger, and a generation counter is what
lets a result computed from the OLD state recognize, after the fact, that it
arrived too late to be trusted.

## The race `clear()` has to defend against

A naive version of this cache, a plain `std::map` behind a mutex with
`clear()` to invalidate, has a real bug. A reader's full sequence is:

1. Look up `key`; miss.
2. Compute `value` (this can take real time: parsing, merging, serializing).
3. Insert `(key, value)`.

If a writer's `clear()` runs between steps 1 and 3, the reader's step-3
insert lands AFTER the clear and repopulates the cache with a value computed
against the state that just became stale -- silently, with no signal that
anything went wrong. The cache now serves wrong data to every subsequent
reader until the NEXT write happens to clear it out again.

`generation_cache` closes this with one counter:

```cpp
Generation generation() const;            // read under the mutex
bool try_insert(Key, Value, Generation);  // commits only if generation matches now
void clear();                             // bumps the generation
```

A reader captures the generation at step 1 (bundled into `get_with_generation`'s
return value, so there is no separate call and no window between the lookup
and the snapshot). At step 3, `try_insert` compares that captured value
against the CURRENT generation under the same mutex `clear()` uses. If a
`clear()` ran in between, the generation moved, the insert is silently
rejected, and the next reader recomputes against the now-current state
instead. The race becomes a detectable, cheap-to-check condition, without
holding a lock across the whole compute.

## One lock, shared for reads

The table and the generation counter are guarded by one reader-writer lock
(`detail::rw_lock`). Readers (`get`, `get_with_generation`, `generation`,
`size`) take it shared; `try_insert` and `clear` take it exclusively, and
`try_insert` checks the generation under that exclusive lock, so the check stays
race-free against `clear()`, the property the whole design depends on. A
reader's table probe is short, so a single lock is enough; there is no
finer-grained locking.

The lock is one atomic word and one `std::mutex`. A reader adds 1 to the word
and, if the writer flag is not set, is in; it leaves by subtracting 1. A writer
takes the mutex, sets the flag with an atomic or, and waits (spinning, then
yielding) until the readers inside have left; it clears the flag and releases
the mutex when it is done. A reader that finds the flag set takes its add back,
waits for the mutex, and adds again while holding it, which cannot meet the flag:
no writer sets it without the mutex. So a waiting writer holds new readers back,
a stream of readers cannot starve a writer, and a writer waits only for the
readers already inside. `std::shared_mutex` makes two mutex round trips per
shared lock on libc++, and leaves its fairness to the platform (glibc prefers
readers); on a short critical section the lock's own cost shows, so this one
spends one atomic operation to enter and one to leave.

## Container choice

No iteration order is exposed, so the container is free. A hashable `Key`
(`std::hash` plus `==`) uses `detail::hash_entries`; any other `Key` uses
`detail::ordered_entries`, a thin `std::map` adapter whose `insert_or_assign`
builds a new node from the value before linking it.

`hash_entries` is an open-addressing table that inserts, replaces, finds and
clears, and never erases one entry, so a slot goes from empty to full and never
back and there are no tombstones. One allocation holds a control byte for each
slot and then the slots, each only a `Key` and a `Value`: no stored hash, no
flag. The capacity is a power of two, the first insert allocates 4 slots and
each growth doubles, and the table holds at most `capacity - max(1, capacity / 8)`
entries: 7/8 of a large table, and an empty slot in every table.

The slots form groups of 16 (SSE2 or NEON) or 8 (a 64-bit word, the portable
fallback and `GENERATION_CACHE_NO_SIMD`); a table smaller than a group is one
group whose spare control bytes hold a value that is neither a tag nor empty.
A key's mixed hash picks its first group by its low bits and a 7-bit tag by its
top bits. A lookup compares the tag with the whole group's control bytes in one
operation, compares the keys of the slots that match (a wrong tag costs a
compare in about 1 in 20 lookups at 100000 keys), and stops at the first group
that has an empty slot. That is enough because an entry is placed in the first
group of its probe sequence that has room, and a slot never empties, so nothing
lies past a group with room. The probe sequence over groups is triangular, which
reaches every group of a power-of-two table, and the load bound keeps a group
with room in the table. A table of 100000 string entries is 131072 slots, 76%
full, and a lookup examines 1.03 groups on average.

The control bytes are the hot part: 1 byte per slot, so 128 KB for the 131072
slots of a 100000-entry table. A lookup reads one group of them, then one slot
(and the key and value storage behind it) however full the table is, instead of
a slot per probe step.

The hash is `std::hash` xor a per-table seed, then a folded 128-bit
multiplication (the high and low halves of the product, xored; `splitmix64`
where the compiler has no 128-bit type). The seed is one value drawn per
process on first use (`detail::process_seed`: `std::random_device`, the clock,
the thread id and an address, mixed; a failing entropy source costs quality, not
an exception, so constructing a `Cache` never throws), varied by the table's
address. It moves the group of keys with different `std::hash` values. It cannot
separate keys whose `std::hash` values are equal, so it does not defend against
a hash function whose collisions can be computed. A growth hashes every key
again, which is why `std::hash` must not throw (a hash requirement).

`insert_or_assign` constructs the entry before it publishes the slot's control
byte and bumps the size, so a throwing `Key` copy leaves the table unchanged. An
overwrite never grows the table. `grow()` allocates the next table beside the
old one and swaps it in only after every entry is placed, so a throw while
allocating or moving leaves the old table untouched. It moves entries when `Key`
and `Value` are both nothrow-move-constructible and copies them otherwise (the
decision is per pair, never per member, so a failed copy never follows a
moved-from key); a move-only type whose move can throw has no safe choice and
gets the basic guarantee, as `std::vector` does. `clear()` destroys the entries,
frees the allocation, and returns to the empty table, so the next insert
allocates 4 slots again.

## Why no built-in size bound

A size cap requires an eviction policy, and this cache deliberately has none
of its own: it isn't recency-based (LRU) or age-based (TTL) by design, and a
count-based cap ("drop the Nth-to-last inserted") would need to rank entries
some other way, reintroducing the complexity this cache exists to avoid.

In the motivating case, the bound comes from upstream: the Detent
distributor this was extracted from only ever composes diffs from a bounded
sliding window of retained history (`max_diff_history` entries), so at most
that many distinct `Key`s are ever reachable at once. The cache is
implicitly bounded by its caller's own domain, without needing to track or
enforce a bound itself. A caller without such a natural bound needs its own
cap, layered on top (e.g. clearing early if `size()` crosses a threshold).
