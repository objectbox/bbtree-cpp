// `bbtree_store::bbtree_store` (16-bbtree-store.md): an ordered store of byte
// strings that threads share, whose every byte lies in pages from its
// allocator. It is bbtree's shared map, whose keys point to their records,
// key and value, and whose leaves keep their records in chains of heap pages
// of their own (`page_allocator_traits::allocate_heap`, a leaf annex, D60).
// An allocator that refuses pages past a budget bounds all the memory a store
// takes; past it, the store refuses inserts (`insert_result::refused`), and
// holds what it held.
//
//   using bbtree_store::bytes_of;
//   bbtree_store::bbtree_store<> store;
//   store.insert(bytes_of("apple"), bytes_of("red"));  // insert_result::inserted
//   store.get(bytes_of("apple"));                      // the bytes of "red"
//
// - An insert writes the value's overflow pages, if it is too long for its
//   record, then finds the key's leaf, held exclusively: a new key's record
//   goes into the leaf's heap, and the key goes in pointing to it; a held
//   key's record takes the new value in place if it fits, or the key points
//   to a new record, and the old one dies. An insert that fails gives the
//   overflow pages back.
// - A removal takes the key out, while the map holds the leaf; its record
//   dies once the map is done with the key, after the removal.
// - A lookup or a scan reads records inside the map's closures, while it
//   holds their leaves latched shared. As only a thread holding a record's
//   leaf exclusively moves, changes or frees it, no record goes while a
//   thread reads it.
//
// Nothing it hands out outlives a call: a lookup copies the value out
// (`get_into`), and a scan's closure sees each entry while its leaf is
// latched.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/node_config.hpp>
#include <bbtree_store/heap_page.hpp>
#include <bbtree_store/leaf_heap.hpp>
#include <bbtree_store/range.hpp>
#include <bbtree_store/record.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bbtree_store {

// The bytes of `text`, as a store takes keys and values: a stateless view,
// for keys and values written as strings.
inline std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// What an insert did, or why it failed; the store holds what it held then.
enum class insert_result : std::uint8_t {
  inserted,        // the key is new
  replaced,        // the key's value was replaced
  key_too_long,    // the key is longer than `max_key_len`
  value_too_long,  // the value is longer than `max_value_len`
  refused,         // the allocator had no page to give
};

// An ordered map of byte strings to byte strings that threads share, whose
// every byte lies in pages from its allocator `A`: the map's nodes, and its
// keys and values, each leaf's in heap pages of its own of config `C`'s heap
// page size, and each value too long for those in heap pages of its own.
template <bbtree::page_allocator A = bbtree::global_allocator, class C = bbtree::default_node_config>
class bbtree_store {
  using store_key = detail::store_key;
  using query_key = detail::query_key;
  using no_value = detail::no_value;
  using map_type = detail::store_map<A, C>;
  using store_leaf = typename map_type::annexed_leaf_type;
  using bytes = std::span<const std::uint8_t>;

 public:
  bbtree_store() noexcept : bbtree_store(A()) {}
  explicit bbtree_store(const A& allocator) noexcept : map_(allocator), heap_(allocator) {}
  bbtree_store(const bbtree_store&) = delete;
  bbtree_store& operator=(const bbtree_store&) = delete;

  ~bbtree_store() {
    // The chains go with the map's leaves. Orphans and overflow pages go as
    // their records die, which only killing them all does.
    if (heap_.kills_at_drop()) {
      map_.scan([&](const store_key& stored, const no_value&) {
        (void)heap_.kill_record(stored.stored_record<C>());
        return true;
      });
    }
  }

  // The longest key: a quarter of a heap page.
  static constexpr std::size_t max_key_len() noexcept { return detail::heap_page<C>::size / 4; }

  // The longest value: 2^32 - 1 bytes, its length's limit.
  static constexpr std::size_t max_value_len() noexcept { return std::numeric_limits<std::uint32_t>::max(); }

  // Appends the value of `key` to `buf`, and returns its length; none without
  // one, `buf` unchanged.
  std::optional<std::size_t> get_into(bytes key, std::vector<std::uint8_t>& buf) const {
    const query_key query(key);
    return map_.read(query.as_store_key(), [&](const store_key& stored, const no_value&) {
      const detail::record_value<C> value = stored.value<C>();
      value.append_to(buf);
      return value.len();
    });
  }

  // A copy of the value of `key`; none without one.
  std::optional<std::vector<std::uint8_t>> get(bytes key) const {
    std::vector<std::uint8_t> buf;
    if (!get_into(key, buf)) {
      return std::nullopt;
    }
    return buf;
  }

  // Whether the store holds `key`.
  bool contains(bytes key) const {
    const query_key query(key);
    return map_.contains(query.as_store_key());
  }

  // Inserts `key` with `value`, or gives the key it holds already `value` in
  // place of its own. Both are copied into the store's heap. Fails, the store
  // holding what it held, on a key longer than `max_key_len`, a value longer
  // than `max_value_len`, or the allocator's refusal of a page, a node or a
  // heap page. Throws `std::bad_alloc`, the store holding what it held, only
  // if the program's heap lacks the memory for a compaction's lists.
  insert_result insert(bytes key, bytes value) {
    if (key.size() > max_key_len()) {
      return insert_result::key_too_long;
    }
    if (value.size() > max_value_len()) {
      return insert_result::value_too_long;
    }
    // The value's overflow pages are `pages`' until a record links them.
    detail::unlinked_overflow<A, C> pages(heap_.allocator());
    try {
      heap_.write_overflow(key.size(), value, pages);
      const query_key query(key);
      return map_.update_or_insert_with_leaf(
          query.as_store_key(), no_value(),
          [&](store_leaf leaf, const store_key& stored, no_value&) {
            heap_.overwrite(leaf, stored, key, value, pages);
            return insert_result::replaced;
          },
          [&](store_leaf leaf, const store_key& fresh, const no_value&) {
            // The record has the new key's bytes, and the key goes into the
            // leaf, which the map holds exclusively, next.
            (void)fresh.replace(heap_.write(leaf, key, value, pages));
            return insert_result::inserted;
          });
    } catch (const bbtree::alloc_error&) {
      return insert_result::refused;
    }
  }

  // Removes `key`; returns whether the store held it.
  bool remove(bytes key) {
    const query_key query(key);
    const auto removed = map_.remove_with_leaf(
        query.as_store_key(), [&](store_leaf leaf, const store_key& stored, const no_value&) {
          heap_.before_removal(leaf, stored);
        });
    if (!removed) {
      return false;
    }
    // The map reads the key until the removal returns: its record dies only
    // now.
    (void)heap_.kill_record(removed->first.template stored_record<C>());
    return true;
  }

  // Runs `visit(key, value)` on each entry whose key lies in `keys`, in key
  // order, with its key and value while its leaf is latched shared, until it
  // returns false. A value with overflow pages is copied into a buffer the
  // scan keeps, for `visit` to see in one piece. As a map's `scan`, it is not
  // a snapshot. Throws `std::invalid_argument` for a range that starts after
  // it ends (`key_range::check`).
  template <class Visit>
  void scan(const key_range& keys, Visit&& visit) const {
    keys.check();
    std::vector<std::uint8_t> gathered;
    // Inline in the map's loop over a leaf's entries: as a call, an entry
    // took a third more instructions, and so fewer entries' record loads
    // were in flight at once (P31).
    const auto in_range = [&](const store_key& stored, const no_value&) __attribute__((always_inline)) {
      const bytes key = stored.key();
      if (!keys.start.admits_from_start(key)) {
        return true;
      }
      if (!keys.end.admits_from_end(key)) {
        return false;
      }
      return static_cast<bool>(visit(key, stored.value<C>().gather(gathered)));
    };
    if (keys.start.kind == bound_kind::unbounded) {
      map_.scan(in_range);
    } else {
      const query_key start(keys.start.key);
      map_.scan_from(start.as_store_key(), in_range);
    }
  }

  // `scan` in reverse key order: from the end of `keys` to its start.
  template <class Visit>
  void scan_rev(const key_range& keys, Visit&& visit) const {
    keys.check();
    std::vector<std::uint8_t> gathered;
    // Inline, as `scan`'s.
    const auto in_range = [&](const store_key& stored, const no_value&) __attribute__((always_inline)) {
      const bytes key = stored.key();
      if (!keys.end.admits_from_end(key)) {
        return true;
      }
      if (!keys.start.admits_from_start(key)) {
        return false;
      }
      return static_cast<bool>(visit(key, stored.value<C>().gather(gathered)));
    };
    if (keys.end.kind == bound_kind::unbounded) {
      map_.scan_rev(in_range);
    } else {
      const query_key end(keys.end.key);
      map_.scan_rev_from(end.as_store_key(), in_range);
    }
  }

  // Copies of the entries whose keys lie in `keys`, key and value, in key
  // order from the front, in reverse from the back, a few at a time
  // (`range_copies`). As `scan`, not a snapshot, and it throws as `scan`
  // does.
  range_copies<bbtree_store> range(const key_range& keys) const {
    keys.check();
    return range_copies<bbtree_store>(*this, keys);
  }

 private:
  map_type map_;
  detail::record_heap<A, C> heap_;
};

}  // namespace bbtree_store
