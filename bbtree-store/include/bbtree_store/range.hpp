// Ranges of a store's keys (`key_range`), and `range_copies`: copies of a
// range of a store's entries, from either end, a few at a time, with no latch
// held between calls.
//
// Each end copies up to `range_copies::batch` entries at a time by a scan
// that starts past what it handed out last, and ends before what the other
// end handed out last. It hands a copy out only if the other end hasn't
// handed it, or anything past it, out since. So, as a scan does, it hands out
// what other threads insert ahead of it, and nothing twice.
#pragma once

#include <bbtree_store/record.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bbtree_store {

// How a range's end is bounded: not at all, at a key it includes, or at one
// it excludes.
enum class bound_kind : std::uint8_t { unbounded, included, excluded };

// One end of a range of keys; its key is borrowed.
struct key_bound {
  bound_kind kind = bound_kind::unbounded;
  std::span<const std::uint8_t> key;

  static key_bound unbounded() noexcept { return {}; }
  static key_bound included(std::span<const std::uint8_t> key) noexcept {
    return {bound_kind::included, key};
  }
  static key_bound excluded(std::span<const std::uint8_t> key) noexcept {
    return {bound_kind::excluded, key};
  }

  // Whether `candidate` lies on the range's side of this bound as its start.
  bool admits_from_start(std::span<const std::uint8_t> candidate) const noexcept {
    return admits(candidate, std::strong_ordering::greater);
  }
  // Whether `candidate` lies on the range's side of this bound as its end.
  bool admits_from_end(std::span<const std::uint8_t> candidate) const noexcept {
    return admits(candidate, std::strong_ordering::less);
  }

 private:
  bool admits(std::span<const std::uint8_t> candidate, std::strong_ordering inside) const noexcept {
    if (kind == bound_kind::unbounded) {
      return true;
    }
    const std::strong_ordering order = detail::byte_order::compare(candidate, key);
    return order == inside || (order == 0 && kind == bound_kind::included);
  }
};

// A range of keys, from `start` to `end`.
struct key_range {
  key_bound start;
  key_bound end;

  // Every key.
  static key_range all() noexcept { return {}; }

  // Whether no key lies in the range.
  bool is_empty() const noexcept {
    if (start.kind == bound_kind::unbounded || end.kind == bound_kind::unbounded) {
      return false;
    }
    const std::strong_ordering order = detail::byte_order::compare(start.key, end.key);
    if (start.kind == bound_kind::included && end.kind == bound_kind::included) {
      return order > 0;
    }
    return order >= 0;
  }

  // Throws `std::invalid_argument`, as std's `BTreeMap::range` panics in the
  // Rust crate, if the range starts after it ends, or starts and ends at the
  // same key, both excluded.
  void check() const {
    if (start.kind == bound_kind::unbounded || end.kind == bound_kind::unbounded) {
      return;
    }
    const std::strong_ordering order = detail::byte_order::compare(start.key, end.key);
    if (order > 0) {
      throw std::invalid_argument("bbtree_store: a range starts after it ends");
    }
    if (order == 0 && start.kind == bound_kind::excluded && end.kind == bound_kind::excluded) {
      throw std::invalid_argument("bbtree_store: a range starts and ends at one excluded key");
    }
  }
};

// A copied entry: its key and value.
using entry_copy = std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>;

// Copies of the entries of a range of a store, in key order from the front,
// in reverse from the back (`bbtree_store::range`). Not a snapshot, as a
// scan isn't.
template <class Store>
class range_copies {
 public:
  // The most entries a copy takes.
  static constexpr std::size_t batch = 16;

  // Copies of `store`'s entries in `range`, which the store checked.
  range_copies(const Store& store, const key_range& range)
      : store_(store), start_(range.start), end_(range.end) {}

  // The next entry from the front; none once the ends meet.
  std::optional<entry_copy> next() { return take(true); }

  // The next entry from the back; none once the ends meet.
  std::optional<entry_copy> next_back() { return take(false); }

 private:
  // A bound that owns its key.
  struct owned_bound {
    bound_kind kind;
    std::vector<std::uint8_t> key;

    explicit owned_bound(const key_bound& bound)
        : kind(bound.kind), key(bound.key.begin(), bound.key.end()) {}
    key_bound borrowed() const noexcept { return {kind, key}; }
  };

  key_range bounds() const noexcept { return {start_.borrowed(), end_.borrowed()}; }

  std::optional<entry_copy> take(bool from_front) {
    std::vector<entry_copy>& ahead = from_front ? front_ : back_;
    if (ahead.empty()) {
      ahead = copy(from_front);
    }
    if (ahead.empty()) {
      return finish();
    }
    entry_copy entry = std::move(ahead.back());
    ahead.pop_back();
    if (!lies_between(entry.first)) {
      return finish();
    }
    owned_bound& passed = from_front ? start_ : end_;
    passed.kind = bound_kind::excluded;
    passed.key = entry.first;
    return entry;
  }

  // Copies up to a batch of entries from the front, or from the back, the
  // next one last.
  std::vector<entry_copy> copy(bool from_front) const {
    std::vector<entry_copy> copied;
    const key_range range = bounds();
    if (range.is_empty()) {
      return copied;
    }
    const auto keep = [&](std::span<const std::uint8_t> key, std::span<const std::uint8_t> value) {
      copied.emplace_back(std::vector<std::uint8_t>(key.begin(), key.end()),
                          std::vector<std::uint8_t>(value.begin(), value.end()));
      return copied.size() < batch;
    };
    if (from_front) {
      store_.scan(range, keep);
    } else {
      store_.scan_rev(range, keep);
    }
    std::reverse(copied.begin(), copied.end());
    return copied;
  }

  // Whether `key` still lies between the ends: the other end may have handed
  // it out, or one past it, since the copy.
  bool lies_between(const std::vector<std::uint8_t>& key) const noexcept {
    return start_.borrowed().admits_from_start(key) && end_.borrowed().admits_from_end(key);
  }

  // Stops both ends: they meet.
  std::optional<entry_copy> finish() {
    front_.clear();
    back_.clear();
    start_.kind = bound_kind::excluded;
    start_.key.clear();
    end_.kind = bound_kind::excluded;
    end_.key.clear();
    return std::nullopt;
  }

  const Store& store_;
  // Where the front goes on: past what it handed out last, or the range's
  // start; the back's, as the front's.
  owned_bound start_;
  owned_bound end_;
  // Entries each end copied ahead, the next one last.
  std::vector<entry_copy> front_;
  std::vector<entry_copy> back_;
};

}  // namespace bbtree_store
