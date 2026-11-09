// Keys and values that tell a test what a map did with them (the Rust crate's
// `DropLedger`, `Tracked` and `CollidingKey`). Rust moves are byte copies and
// its test types are not `Clone`, so its tests cannot copy an entry by
// accident or move one the wrong way. In C++ a container picks copy or move
// by overload resolution and moves by hand, so these types tell them apart:
//
// - Every logical object has an id in an `object_ledger`. A move keeps the
//   id, a copy gets a new one, so a test that compares ids sees whether the
//   map holds the very object it was given.
// - A destruction of the same object twice, a read of a moved-from or
//   destroyed object, and a byte-move of an object that must move one at a
//   time end the test's process with the rule that broke
//   (`lifetime_violation`); a leak shows in `object_ledger::nothing_left`.
// - A copy can be made to throw (`refuse_copies_after`), the C++ analogue of
//   the Rust tests' panicking `Clone`.
#pragma once

#include <bbtree/inline_key.hpp>
#include <bbtree/relocation.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <utility>
#include <vector>

namespace bbtree_test {

// A broken rule of object lifetime or page ownership. It is found in
// destructors and `noexcept` moves, where a test can't throw, and what
// follows it would be undefined: the process ends, which fails the test.
struct lifetime_violation {
  [[noreturn]] static void report(const char* rule) noexcept {
    std::fprintf(stderr, "lifetime violation: %s\n", rule);
    std::fflush(stderr);
    std::abort();
  }
};

// Thrown by a copy the ledger was told to refuse.
struct copy_refused : std::exception {};

// The book of every tracked object: which are alive, how many objects
// (moved-from ones included) exist, and how many copies may still be made.
class object_ledger {
 public:
  using object_id = std::uint64_t;
  // The id of a moved-from object.
  static constexpr object_id none = 0;

  object_ledger() = default;
  object_ledger(const object_ledger&) = delete;
  object_ledger& operator=(const object_ledger&) = delete;

  object_id issue() {
    alive_.push_back(true);
    ++live_;
    return alive_.size() - 1;
  }
  void retire(object_id id) noexcept {
    if (!is_live(id)) {
      lifetime_violation::report("an object was destroyed twice");
    }
    alive_[id] = false;
    --live_;
  }
  bool is_live(object_id id) const noexcept { return id != none && id < alive_.size() && alive_[id]; }

  void object_constructed() noexcept { ++objects_; }
  void object_destroyed() noexcept { --objects_; }

  // Lets `count` more copies through; the ones after throw `copy_refused`.
  void refuse_copies_after(long count) noexcept { copies_left_ = count; }
  void allow_copies() noexcept { copies_left_ = unlimited; }
  void spend_copy() {
    if (copies_left_ == 0) {
      throw copy_refused();
    }
    if (copies_left_ > 0) {
      --copies_left_;
    }
  }

  // How many logical objects are alive.
  long live() const noexcept { return live_; }
  // Whether every object was destroyed, the moved-from ones too.
  bool nothing_left() const noexcept { return live_ == 0 && objects_ == 0; }

 private:
  static constexpr long unlimited = -1;

  std::vector<bool> alive_{false};
  long live_ = 0;
  long objects_ = 0;
  long copies_left_ = unlimited;
};

// How the nodes may move an object (`relocation.hpp`).
enum class moves : std::uint8_t {
  // By move construction and destruction only: the object knows its address.
  one_at_a_time,
  // As its bytes: declared trivially relocatable.
  as_bytes,
};

// Whether an object can be copied.
enum class copies : std::uint8_t { allowed, deleted };

// The tracked part of a key or value.
template <moves Moves, copies Copies>
class tracked_object {
 public:
  explicit tracked_object(object_ledger& ledger) : ledger_(&ledger), id_(ledger.issue()), self_(this) {
    ledger_->object_constructed();
  }
  tracked_object(const tracked_object& other)
    requires(Copies == copies::allowed)
      : ledger_(other.ledger_), self_(this) {
    other.require_live();
    ledger_->spend_copy();
    id_ = ledger_->issue();
    ledger_->object_constructed();
  }
  tracked_object(tracked_object&& other) noexcept : ledger_(other.ledger_), self_(this) {
    other.require_object();
    id_ = std::exchange(other.id_, object_ledger::none);
    ledger_->object_constructed();
  }
  tracked_object& operator=(const tracked_object& other)
    requires(Copies == copies::allowed)
  {
    if (this != &other) {
      other.require_live();
      require_object();
      ledger_->spend_copy();
      retire();
      id_ = ledger_->issue();
    }
    return *this;
  }
  tracked_object& operator=(tracked_object&& other) noexcept {
    if (this != &other) {
      other.require_object();
      require_object();
      retire();
      id_ = std::exchange(other.id_, object_ledger::none);
    }
    return *this;
  }
  ~tracked_object() {
    require_object();
    retire();
    ledger_->object_destroyed();
    state_ = destroyed;
  }

  object_ledger::object_id id() const noexcept {
    require_live();
    return id_;
  }
  bool moved_from() const noexcept {
    require_object();
    return id_ == object_ledger::none;
  }
  object_ledger& ledger() const noexcept { return *ledger_; }

  // That this storage holds an object, moved-from or not, where it was
  // constructed.
  void require_object() const noexcept {
    if (state_ != constructed) {
      lifetime_violation::report(
          "storage was read as an object after its destructor, or before any constructor");
    }
    if (Moves == moves::one_at_a_time && self_ != this) {
      lifetime_violation::report("an object that is not trivially relocatable was moved as bytes");
    }
  }
  void require_live() const noexcept {
    require_object();
    if (!ledger_->is_live(id_)) {
      lifetime_violation::report("a moved-from object was read");
    }
  }

 private:
  static constexpr std::uint32_t constructed = 0xA11CE5ED;
  static constexpr std::uint32_t destroyed = 0xDEADDEAD;

  void retire() noexcept {
    if (id_ != object_ledger::none) {
      ledger_->retire(id_);
      id_ = object_ledger::none;
    }
  }

  object_ledger* ledger_;
  object_ledger::object_id id_ = object_ledger::none;
  const tracked_object* self_;
  std::uint32_t state_ = constructed;
};

// The inline keys a tracked key gives its nodes (`inline_key.hpp`).
enum class key_window : std::uint8_t {
  // None: internal nodes keep the keys, and searches compare them.
  none,
  // The low two bytes of its order, as the std byte arrays' nodes keep.
  two_bytes,
  // Eight bytes: a separator's key lives in its leaf's head (D49).
  eight_bytes,
};

// A 64-byte key ordered by one number, so that few entries make a tall tree.
template <moves Moves, key_window Window, copies Copies = copies::allowed>
class tracked_key {
 public:
  tracked_key(object_ledger& ledger, std::uint64_t order) : object_(ledger), order_(order) {}

  std::uint64_t order() const noexcept {
    object_.require_live();
    return order_;
  }
  object_ledger::object_id id() const noexcept { return object_.id(); }
  bool moved_from() const noexcept { return object_.moved_from(); }
  object_ledger& ledger() const noexcept { return object_.ledger(); }

  friend bool operator==(const tracked_key& a, const tracked_key& b) noexcept {
    return a.order() == b.order();
  }
  friend std::strong_ordering operator<=>(const tracked_key& a, const tracked_key& b) noexcept {
    return a.order() <=> b.order();
  }

 private:
  tracked_object<Moves, Copies> object_;
  std::uint64_t order_;
  unsigned char padding_[64 - sizeof(tracked_object<Moves, Copies>) - sizeof(std::uint64_t)] = {};
};

template <moves Moves, copies Copies = copies::allowed>
class tracked_value {
 public:
  tracked_value(object_ledger& ledger, std::uint64_t payload) : object_(ledger), payload_(payload) {}

  std::uint64_t payload() const noexcept {
    object_.require_live();
    return payload_;
  }
  object_ledger::object_id id() const noexcept { return object_.id(); }
  bool moved_from() const noexcept { return object_.moved_from(); }

  friend bool operator==(const tracked_value& a, const tracked_value& b) noexcept {
    return a.payload() == b.payload();
  }

 private:
  tracked_object<Moves, Copies> object_;
  std::uint64_t payload_;
};

}  // namespace bbtree_test

template <bbtree_test::key_window Window, bbtree_test::copies Copies>
struct bbtree::is_trivially_relocatable<
    bbtree_test::tracked_key<bbtree_test::moves::as_bytes, Window, Copies>> : std::true_type {};

template <bbtree_test::copies Copies>
struct bbtree::is_trivially_relocatable<bbtree_test::tracked_value<bbtree_test::moves::as_bytes, Copies>>
    : std::true_type {};

// A tracked key's inline keys: the big-endian bytes of its order.
template <bbtree_test::moves Moves, bbtree_test::copies Copies>
struct bbtree::inline_key<bbtree_test::tracked_key<Moves, bbtree_test::key_window::eight_bytes, Copies>> {
  using key_type = bbtree_test::tracked_key<Moves, bbtree_test::key_window::eight_bytes, Copies>;
  static bbtree::inline_word at(const key_type& key, std::size_t offset) noexcept {
    return offset >= 8 ? 0 : key.order() << (8 * offset);
  }
};

template <bbtree_test::moves Moves, bbtree_test::copies Copies>
struct bbtree::inline_key<bbtree_test::tracked_key<Moves, bbtree_test::key_window::two_bytes, Copies>> {
  using key_type = bbtree_test::tracked_key<Moves, bbtree_test::key_window::two_bytes, Copies>;
  static constexpr bbtree::inline_width width = bbtree::inline_width::two;
  static bbtree::inline_word at(const key_type& key, std::size_t offset) noexcept {
    return offset >= 8 ? 0 : key.order() << (8 * offset);
  }
};
