# Architecture

The internals of `generation_cache`: one mutex guarding a map and a counter.
For usage see `README.md`; for the repo map and invariants see `AGENTS.md`.

## Shape

A single header, nothing else:

```
  generation_cache.hh   the templated Cache<Key, Value> class
```

No compiled translation unit, no external dependency. `generation_cache.hh`
includes only `<map>`, `<mutex>`, `<cstdint>`, `<optional>`, and `<utility>`.

## The problem this solves

A cache keyed by `Key` producing a `Value` usually has to answer: when is an
entry no longer good? Two common answers are "after N seconds" (TTL) and
"when something more recently used needs the slot" (LRU). Both rank entries
individually.

The motivating case here doesn't fit either model. Picture a server with one
piece of mutable state (a version counter, say) that changes in discrete
steps -- an accepted write, a new commit. Many readers derive a `Value` (e.g.
a diff from some `Key`, a base version, to the current state) from that
state. Every `Value` computed before step N is valid until step N+1 arrives,
and then EVERY one of them is wrong at once, regardless of how recently or
often each was used. There is no partial staleness and no useful recency
ordering -- an entry is either exactly current or completely wrong.

So the right invalidation unit isn't the entry, it's the whole cache, and the
right trigger isn't time or access pattern, it's "the thing I was computed
from changed." `clear()` is that trigger, and a generation counter is what
lets a result computed from the OLD state recognize, after the fact, that it
arrived too late to be trusted.

## The race `clear()` has to defend against

A naive version of this cache -- a plain `std::map` behind a mutex, with
`clear()` to invalidate -- has a real bug. A reader's full sequence is:

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
instead. The race isn't prevented by making the reader faster or taking a
bigger lock across the whole compute -- it is turned into a detectable,
cheap-to-check condition instead.

## Why one mutex, not finer-grained locking

The map and the generation counter are guarded by the same `std::mutex`,
and every public method takes it for its whole body. This is deliberately not
a `shared_mutex` with reader/writer separation: the work this cache is meant
to short-circuit (diff composition, patch merging, serialization) is
expected to dominate actual map access time by orders of magnitude, so
contention on the map itself is not where the cost lives. Keeping one mutex
keeps `try_insert`'s generation check trivially race-free against `clear()`
-- the property the whole design depends on -- rather than trading that
simplicity for reader/writer parallelism the use case doesn't need.

## Why no built-in size bound

A size cap requires an eviction policy, and this cache deliberately has none
of its own: it isn't recency-based (LRU) or age-based (TTL) by design, and a
count-based cap ("drop the Nth-to-last inserted") would need to rank entries
some other way, reintroducing the complexity this cache exists to avoid.

In the motivating case, the bound comes from upstream: the Detent
distributor this was extracted from only ever composes diffs from a bounded
sliding window of retained history (`max_diff_history` entries), so at most
that many distinct `Key`s are ever reachable at once -- the cache is
implicitly bounded by its caller's own domain, without needing to track or
enforce a bound itself. A caller without such a natural bound needs its own
cap, layered on top (e.g. clearing early if `size()` crosses a threshold).
