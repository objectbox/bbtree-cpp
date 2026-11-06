// Types for the tests: values whose every construction and destruction a
// ledger counts (the Rust crate's `DropLedger` and `Tracked`), and a string
// key with its own `inline_key` specialization.
#pragma once

#include <bbtree/inline_key.hpp>

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace bbtree_test {

// Counts the tracked values alive; every one must be destroyed once.
class drop_ledger {
 public:
  void constructed() noexcept { ++alive_; }
  void destroyed() noexcept { --alive_; }
  long alive() const noexcept { return alive_; }

 private:
  long alive_ = 0;
};

// A value that reports to its ledger, and is not trivially relocatable: it
// moves one object at a time through the nodes.
class tracked {
 public:
  tracked(drop_ledger& ledger, std::uint64_t id) noexcept : ledger_(&ledger), id_(id) {
    ledger_->constructed();
  }
  tracked(const tracked& other) noexcept : ledger_(other.ledger_), id_(other.id_) { ledger_->constructed(); }
  tracked(tracked&& other) noexcept : ledger_(other.ledger_), id_(other.id_) { ledger_->constructed(); }
  tracked& operator=(const tracked& other) noexcept {
    id_ = other.id_;
    return *this;
  }
  tracked& operator=(tracked&& other) noexcept {
    id_ = other.id_;
    return *this;
  }
  ~tracked() { ledger_->destroyed(); }

  std::uint64_t id() const noexcept { return id_; }
  friend bool operator==(const tracked& a, const tracked& b) noexcept { return a.id_ == b.id_; }

 private:
  drop_ledger* ledger_;
  std::uint64_t id_;
};

// A string key whose user gave it inline keys, with hash tags and comparisons
// from where keys differ: the trait row of the node plan (D36, D43).
struct user_string {
  std::string text;
  friend auto operator<=>(const user_string&, const user_string&) = default;
  friend bool operator==(const user_string&, const user_string&) = default;

  std::span<const std::uint8_t> bytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  }
};

}  // namespace bbtree_test

template <>
struct bbtree::inline_key<bbtree_test::user_string> {
  static bbtree::inline_word at(const bbtree_test::user_string& key, std::size_t offset) noexcept {
    return bbtree::detail::big_endian::window(key.bytes(), offset);
  }
  static std::uint8_t hash_tag(const bbtree_test::user_string& key) noexcept {
    return key.text.size() < 255 ? static_cast<std::uint8_t>(key.text.size()) : std::uint8_t{255};
  }
  static std::strong_ordering cmp_from(const bbtree_test::user_string& a, const bbtree_test::user_string& b,
                                       std::size_t equal_len) noexcept {
    return bbtree::detail::search_bytes(a.bytes()).cmp_from(b.bytes(), equal_len);
  }
};
