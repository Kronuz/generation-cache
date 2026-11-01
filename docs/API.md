# generation-cache API

```cpp
namespace generation_cache {

template <typename Key, typename Value>
class Cache {
public:
    using Generation = std::uint64_t;

    struct Lookup {
        std::optional<Value> value;
        Generation generation;
    };

    Generation generation() const;
    std::optional<Value> get(const Key& key) const;
    Lookup get_with_generation(const Key& key) const;
    bool try_insert(const Key& key, Value value, Generation at_generation);
    void clear();
    std::size_t size() const;
};

} // namespace generation_cache
```

- **`generation()`**: the current generation, to snapshot it separately from a
  lookup.
- **`get(key)`**: a plain lookup; `std::nullopt` on a miss.
- **`get_with_generation(key)`**: a lookup plus the generation it was read at,
  for a caller about to recompute on a miss.
- **`try_insert(key, value, at_generation)`**: commits only if `at_generation`
  still matches the current generation; returns whether it committed. A stale
  call never throws and never overwrites a current entry.
- **`clear()`**: drops every entry and advances the generation.
- **`size()`**: the current entry count.

## The reader pattern

```cpp
auto lookup = cache.get_with_generation(key);
if (lookup.value) {
    return *lookup.value;  // hit, no recompute
}
auto value = expensive_compute(key);  // reads whatever the current state is
cache.try_insert(key, value, lookup.generation);
return value;
```

The writer calls `cache.clear()` whenever the source state changes. If a
`clear()` lands between `get_with_generation` and `try_insert` (because
`expensive_compute` took a while), the insert is dropped, and the next reader
recomputes against the new state instead of seeing a value computed from the
old one.

## Thread safety

All methods are thread-safe. One internal reader-writer lock guards the table
and the generation counter together, so a lookup and a `clear()` can never
interleave inconsistently. `get`, `get_with_generation`, `generation` and `size`
take it shared, so readers run in parallel: a reader enters with one atomic add
and leaves with one subtract, so a lookup in a small cache costs about what the
lookup itself costs. `try_insert` and `clear` take it exclusively and wait only
for the readers already inside; a waiting writer holds new readers back, so
readers that overlap continuously cannot starve it, on every platform. A `Value`
copy constructor, `std::hash` and `operator==` run under the lock and must not
call back into the same cache.

## Exception safety

`try_insert` has the strong exception guarantee: if anything throws (a `Key`
copy, a `Value` move or copy, an allocation, the table's rehash), the cache is
unchanged and still usable. The rehash builds the new table completely before it
replaces the old one, and moves entries only when `Key` and `Value` are
nothrow-move-constructible, copying them otherwise. A move-only type whose move
can throw gets the basic guarantee instead, as with `std::vector`, and
overwriting an existing key assigns `Value`, which is strong only if its move
assignment cannot throw. Constructing a `Cache` never throws.

## Storage and hashing

Entries have no exposed order. When `Key` has a `std::hash` and `operator==`,
they live in a small open-addressing table: linear probing, load <= 0.5, the
64-bit hash stored per slot, and 4 slots allocated by the first insert. The hash
is mixed with a seed drawn once per process (from `std::random_device`, mixed
with the clock, the thread id and an address, so an entropy source that fails
costs quality and never an exception) and varied per table, so the index of a
key is not a function of `std::hash` alone. The seed spreads the indexes of keys
whose `std::hash` values differ. Keys with equal `std::hash` values collide in
every cache, and the standard library's string hashes are unseeded, so a caller
that must withstand crafted keys needs its own `std::hash`. A `Key` with only
`operator<` uses `std::map`.
