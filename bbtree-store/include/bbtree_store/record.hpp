// A store's records (`record`) and the map's keys that point to them
// (`store_key`).
//
// A record is a key and its value, in a heap page (`heap_page.hpp`):
//
//   | key_len: u32 | value_len: u32 | generation: u8 | key ... | value ... |
//
// native-endian and unaligned, the generation that of the leaf whose heap it
// was written into (`leaf_heap.hpp`). A value too long for its record to fit
// in a heap page's room goes into overflow pages (`overflow.hpp`), the record
// keeping a link to them after the key, then the value's start:
//
//   | key_len: u32 | value_len: u32 | generation: u8 | key ... | link | start ... |
//
// The pages are all full, the start what is left over, as SQLite does, if it
// fits; else the record keeps none of the value, and the last page holds
// what is left. Which of them a record is, and how much it keeps, its lengths
// tell (`record::kept_len`). A record keeps at least a link's room after its
// key, so that it can become a stub in place:
//
//   | key_len: u32 | value_len: u32 | generation: u8 + 0x80 | key ... | target |
//
// whose value is the record at `target`'s, the stub's key's too. A stub is
// the record of a leaf's head key that took a value too long for it (P24):
// the head key is a separator, which threads read without holding its leaf,
// so its record stays where it is, unchanged, while it is the head.
//
// The map orders its keys, and keeps their inline keys, by their key bytes,
// as it does a byte string's. It reads a key's bytes only while it holds the
// key's leaf latched, or, for a head, the internal node its separator is in:
// its unlatched reads settle a key from the inline keys and hash tags they
// stored, and never call a key's own code (D57: a `store_key` doesn't order
// by its own bytes). So a record, and its overflow pages, stay put while any
// thread reads them, if the store moves, changes or frees them only while it
// holds their leaf exclusively, and never a head's (`leaf_heap.hpp`).
#pragma once

#include <bbtree/inline_key.hpp>
#include <bbtree_store/heap_page.hpp>
#include <bbtree_store/overflow.hpp>

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace bbtree_store::detail {

using bytes = std::span<const std::uint8_t>;

// How byte strings order: lexicographically, a prefix first.
struct byte_order {
  static std::strong_ordering compare(bytes a, bytes b) noexcept {
    const std::size_t both = a.size() < b.size() ? a.size() : b.size();
    const int by_bytes = both == 0 ? 0 : std::memcmp(a.data(), b.data(), both);
    if (by_bytes != 0) {
      return by_bytes < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return a.size() <=> b.size();
  }
};

// A leaf's generation (`leaf_heap.hpp`): 7 bits, wrapping at 128, as a
// record's generation byte keeps it beside the stub bit.
struct generation_count {
  static constexpr std::uint8_t mask = 0x7f;

  static std::uint8_t next(std::uint8_t generation) noexcept {
    return static_cast<std::uint8_t>((generation + 1) & mask);
  }
  // How many generations `later` is past `earlier`, wrapping.
  static std::uint8_t since(std::uint8_t later, std::uint8_t earlier) noexcept {
    return static_cast<std::uint8_t>((later - earlier) & mask);
  }
};

// A record's first fields, the same in every node config: where a record's
// key lies, and the length no record has, which a lookup's key starts with
// (`query_key`). A separator's readers read these alone.
struct record_fields {
  static constexpr std::size_t len_size = sizeof(std::uint32_t);
  // Where the key starts: after both lengths and the generation.
  static constexpr std::size_t header_size = 2 * len_size + sizeof(std::uint8_t);
  static constexpr std::uint32_t query_marker = std::numeric_limits<std::uint32_t>::max();

  static std::uint32_t len_at(const std::byte* record, std::size_t offset) noexcept {
    std::uint32_t len;
    std::memcpy(&len, record + offset, sizeof len);
    return len;
  }
  static void write_len(std::byte* record, std::size_t offset, std::size_t len) noexcept {
    const auto narrow = static_cast<std::uint32_t>(len);
    std::memcpy(record + offset, &narrow, sizeof narrow);
  }
  static bytes key(const std::byte* record) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(record + header_size), len_at(record, 0)};
  }
};

// A record's value, for as long as the caller reads the record: the bytes
// its record keeps, then its overflow pages'.
template <class C>
class record_value {
 public:
  record_value(bytes start, overflow<C> rest, std::size_t rest_len) noexcept
      : start_(start), rest_(rest), rest_len_(rest_len) {}

  std::size_t len() const noexcept { return start_.size() + rest_len_; }
  bool has_overflow() const noexcept { return static_cast<bool>(rest_); }
  overflow<C> overflow_pages() const noexcept { return rest_; }
  // The value's start, in its record: all of it, without overflow pages.
  bytes start() const noexcept { return start_; }

  // Appends the value to `buf`.
  void append_to(std::vector<std::uint8_t>& buf) const {
    if (rest_) {
      buf.reserve(buf.size() + len());
    }
    buf.insert(buf.end(), start_.begin(), start_.end());
    if (rest_) {
      rest_.append_to(rest_len_, buf);
    }
  }

  // The value's bytes: the record's, or, with overflow pages, those of
  // `gathered`, cleared and filled with them.
  bytes gather(std::vector<std::uint8_t>& gathered) const {
    if (!rest_) {
      return start_;
    }
    gathered.clear();
    append_to(gathered);
    return gathered;
  }

 private:
  bytes start_;
  overflow<C> rest_;
  std::size_t rest_len_;
};

// A record in a heap page of node config `C`, by its address: valid while a
// key, or a stub, points to it, read while its leaf is held (top of the
// file).
template <class C>
class record {
  static constexpr std::size_t len_size = record_fields::len_size;
  static constexpr std::size_t header_size = record_fields::header_size;
  static constexpr std::size_t generation_offset = 2 * len_size;
  // The bit of the generation byte that marks a stub.
  static constexpr std::uint8_t stub_bit = 0x80;
  // The size of the link to a value's overflow pages, or to a stub's target.
  static constexpr std::size_t link_size = sizeof(std::byte*);

 public:
  // The record at `at`, where a record lies.
  explicit record(std::byte* at) noexcept : at_(at) {}

  std::byte* address() const noexcept { return at_; }

  // The generation of the leaf whose heap the record was written into.
  std::uint8_t generation() const noexcept { return generation_byte() & generation_count::mask; }

  bytes key() const noexcept { return record_fields::key(at_); }

  // How many of the bytes of a value of `value_len` its record keeps, with a
  // key of `key_len`: all of them if the record fits in a heap page's room;
  // else, past the link to its overflow pages, what is left over once they
  // are full, if it fits; else none.
  static constexpr std::size_t kept_len(std::size_t key_len, std::size_t value_len) noexcept {
    constexpr std::size_t room = heap_page<C>::room_for_records;
    if (header_size + key_len + std::max(value_len, link_size) <= room) {
      return value_len;
    }
    const std::size_t left_over = value_len % overflow<C>::room;
    return header_size + key_len + link_size + left_over <= room ? left_over : 0;
  }

  // The size of the record of a key and a value of these lengths: at most a
  // heap page's room, for a key no longer than a quarter page; at least a
  // link's room after the key.
  static constexpr std::size_t size_of(std::size_t key_len, std::size_t value_len) noexcept {
    const std::size_t kept = kept_len(key_len, value_len);
    return header_size + key_len + std::max((kept < value_len ? link_size : 0) + kept, link_size);
  }

  // Writes the record of `key` and `value`, written in a leaf of
  // `generation`, at `to`, where `size_of` bytes are writable, apart from
  // `key` and `value`; `pages` holds the bytes of the value it doesn't keep
  // (`kept_len`), if there are any.
  static record write(std::byte* to, bytes key, bytes value, overflow<C> pages,
                      std::uint8_t generation) noexcept {
    const std::size_t kept = kept_len(key.size(), value.size());
    assert(static_cast<bool>(pages) == (kept < value.size()) &&
           "overflow pages hold what a record doesn't keep of its value");
    record_fields::write_len(to, 0, key.size());
    record_fields::write_len(to, len_size, value.size());
    to[generation_offset] = static_cast<std::byte>(generation & generation_count::mask);
    std::byte* at = to + header_size;
    copy(at, key.data(), key.size());
    at += key.size();
    if (pages) {
      std::byte* link = pages.link();
      std::memcpy(at, &link, link_size);
      at += link_size;
    }
    copy(at, value.data(), kept);
    return record(to);
  }

  // The record's size, in its page.
  std::size_t size() const noexcept {
    return is_stub() ? header_size + key_len() + link_size : size_of(key_len(), value_len());
  }

  // Whether the record is a stub, whose value is its target's (top of the
  // file).
  bool is_stub() const noexcept { return (generation_byte() & stub_bit) != 0; }

  // A stub's target: the record that holds its value.
  record target() const noexcept {
    assert(is_stub());
    std::byte* to = nullptr;
    std::memcpy(&to, at_ + header_size + key_len(), link_size);
    return record(to);
  }

  // Makes this record, not a stub, a stub of `to`, a record of its key, in
  // place: it shrinks to a stub's size, its overflow pages, if any, the
  // caller's to free. The record's key, and its key's length, stay as they
  // are, for the threads that read them without holding its leaf.
  void become_stub_of(record to) const noexcept {
    assert(!is_stub() && "a stub is made of a record");
    at_[generation_offset] = static_cast<std::byte>(generation_byte() | stub_bit);
    retarget(to);
  }

  // Points a stub to `to`, its target moved or replaced.
  void retarget(record to) const noexcept {
    assert(is_stub());
    std::byte* address = to.address();
    std::memcpy(at_ + header_size + key_len(), &address, link_size);
    record_fields::write_len(at_, len_size, to.value_len());
  }

  // Not recursive, so that it inlines into a scan's visit: a stub's target,
  // which holds the value, is no stub.
  record_value<C> value() const noexcept {
    const record holder = is_stub() ? target() : *this;
    assert(!holder.is_stub() && "a stub's target is a record");
    const std::size_t key_size = holder.key_len();
    const std::size_t value_size = holder.value_len();
    const std::size_t kept = kept_len(key_size, value_size);
    const std::byte* after_key = holder.at_ + header_size + key_size;
    if (kept == value_size) {
      return {as_bytes(after_key, value_size), overflow<C>(), 0};
    }
    std::byte* link = nullptr;
    std::memcpy(&link, after_key, link_size);
    assert(link != nullptr && "a record links its pages");
    return {as_bytes(after_key + link_size, kept), overflow<C>(link), value_size - kept};
  }

  // The value's overflow pages, a stub's target's; none if the record keeps
  // it whole.
  overflow<C> overflow_pages() const noexcept { return value().overflow_pages(); }

  // Writes a copy of the record at `to`, where `size` bytes are writable,
  // apart from the record, with `generation`: the copy links the record's
  // overflow pages, or a stub's target, which are its then.
  record copy_to(std::byte* to, std::uint8_t generation) const noexcept {
    std::memcpy(to, at_, size());
    to[generation_offset] =
        static_cast<std::byte>((generation_byte() & stub_bit) | (generation & generation_count::mask));
    return record(to);
  }

  // Gives the record, not a stub, `value` in place of its own, which it keeps
  // whole, and which is no shorter: the bytes past the new value are garbage.
  // The caller holds the record's leaf exclusively: no thread reads its
  // value, and its key stays as it is.
  void overwrite_value(bytes value) const noexcept {
    [[maybe_unused]] const record_value<C> old = this->value();
    assert(!is_stub() && !old.has_overflow() && value.size() <= old.start().size() &&
           "a value overwrites one kept whole, no shorter");
    record_fields::write_len(at_, len_size, value.size());
    copy(at_ + header_size + key_len(), value.data(), value.size());
  }

 private:
  std::uint8_t generation_byte() const noexcept { return static_cast<std::uint8_t>(at_[generation_offset]); }
  std::size_t key_len() const noexcept { return record_fields::len_at(at_, 0); }
  // The value's length, overflow pages and all.
  std::size_t value_len() const noexcept { return record_fields::len_at(at_, len_size); }

  static bytes as_bytes(const std::byte* at, std::size_t len) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(at), len};
  }
  static void copy(std::byte* to, const std::uint8_t* from, std::size_t len) noexcept {
    if (len > 0) {
      std::memcpy(to, from, len);
    }
  }

  std::byte* at_;
};

class query_key;

// A store's key: a pointer to its record, which the store swaps for another
// of the same key, in place, while it holds the key's leaf exclusively, as
// the value changes or the record moves; never a leaf's head key's (top of
// the file). The map hands keys out as `const`; the pointer is `mutable`
// (PORTING.md, P24).
//
// Not generic over the store's node config: only reading its record's value
// needs it (`value`). A generic key made the Rust store's get 45 ns slower
// at 10M keys, for eight instructions more (2026-10-21).
class store_key {
 public:
  // The record the key points to, in a store of node config `C`.
  template <class C>
  record<C> stored_record() const noexcept {
    assert(record_ != nullptr && "a key has a record");
    return record<C>(record_);
  }

  // Points the key to `to`, a record of the same key, and returns the one it
  // pointed to. The record stays where it is, unchanged, while the key points
  // to it and a thread may read it; the caller holds the key's leaf
  // exclusively, so that no thread reads the key meanwhile, and the key is not
  // the leaf's head.
  template <class C>
  record<C> replace(record<C> to) const noexcept {
    std::byte* old = record_;
    record_ = to.address();
    return record<C>(old);
  }

  // The key's bytes.
  bytes key() const noexcept;

  // The value, in a store of node config `C`; not for a lookup's key, which
  // has none.
  template <class C>
  record_value<C> value() const noexcept {
    assert(record_fields::len_at(record_, 0) != record_fields::query_marker && "a lookup's key has no value");
    return stored_record<C>().value();
  }

  friend bool operator==(const store_key& a, const store_key& b) noexcept {
    return byte_order::compare(a.key(), b.key()) == 0;
  }
  friend std::strong_ordering operator<=>(const store_key& a, const store_key& b) noexcept {
    return byte_order::compare(a.key(), b.key());
  }

 private:
  friend class query_key;
  explicit store_key(std::byte* at) noexcept : record_(at) {}

  mutable std::byte* record_;
};

// A lookup's key, or a new key's before its record is written, where the
// caller has it: the map looks a `store_key` up with inline keys only when
// the query is a `store_key` too, which `as_store_key` makes. It starts with
// a length no record has, which tells it from a record.
class query_key {
 public:
  explicit query_key(bytes key) noexcept : key_(key) {}
  query_key(const query_key&) = delete;
  query_key& operator=(const query_key&) = delete;

  // A store key that points to this query, valid while it lives: the caller
  // drops the store key first, or points it to a record.
  store_key as_store_key() const noexcept {
    return store_key(reinterpret_cast<std::byte*>(const_cast<query_key*>(this)));
  }

  bytes key() const noexcept { return key_; }

 private:
  // `record_fields::query_marker`, where a record's key length is. Read
  // only through the store key's pointer (`store_key::key`), which newer
  // Clang doesn't see.
  [[maybe_unused]] std::uint32_t marker_ = record_fields::query_marker;
  bytes key_;
};

inline bytes store_key::key() const noexcept {
  if (record_fields::len_at(record_, 0) == record_fields::query_marker) {
    // A pointer whose first length is the marker points to a `query_key`,
    // which outlives it (`query_key::as_store_key`).
    return reinterpret_cast<const query_key*>(record_)->key();
  }
  return record_fields::key(record_);
}

// A map value that takes no room: a store's entries are its keys.
struct no_value {};

}  // namespace bbtree_store::detail

// A store key's inline keys are its key bytes', as a byte string's: 8-byte
// windows, its length as its hash tag, comparisons from where keys differ and
// against a prefix.
template <>
struct bbtree::inline_key<bbtree_store::detail::store_key> {
  using bytes_key = bbtree::inline_key<std::span<const std::uint8_t>>;
  using store_key = bbtree_store::detail::store_key;

  static bbtree::inline_word at(const store_key& key, std::size_t offset) noexcept {
    return bytes_key::at(key.key(), offset);
  }
  static std::uint8_t hash_tag(const store_key& key) noexcept { return bytes_key::hash_tag(key.key()); }
  static std::strong_ordering cmp_from(const store_key& a, const store_key& b,
                                       std::size_t equal_len) noexcept {
    return bytes_key::cmp_from(a.key(), b.key(), equal_len);
  }
  static std::strong_ordering cmp_prefix(const store_key& key,
                                         std::span<const std::uint8_t> prefix) noexcept {
    return bytes_key::cmp_prefix(key.key(), prefix);
  }
};
