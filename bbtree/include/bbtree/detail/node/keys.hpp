// Keys inside the nodes: how a map's keys order, whether and how they get
// inline keys (D18), what each node kind stores beside them (the node plan,
// D47), and what a search looks for.
//
// - `key_order` compares keys three ways with the map's comparator.
// - `key_inliner` resolves, at compile time, how keys get inline keys: as std
//   byte strings, through an `inline_key` specialization, or not at all.
// - `node_plan` is what one node kind stores beside its keys and how a search
//   tells keys apart (07-node-plan.md's table).
// - `window` is an inline key with the prefix length it is taken at.
// - `windowed_key` is a key outside the nodes with its window;
//   `separator` a separator on its way between nodes.
// - `search_key` is what a search looks for.
#pragma once

#include <bbtree/detail/node/layout.hpp>
#include <bbtree/inline_key.hpp>
#include <bbtree/leaf_annex.hpp>
#include <bbtree/node_config.hpp>

#include <algorithm>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>

namespace bbtree::detail {

// How keys order under the map's comparator, three ways. Comparators are
// stateless (PORTING.md, P8), so one is made where it is needed.
template <class Compare>
struct key_order {
  static_assert(std::is_empty_v<Compare> && std::is_default_constructible_v<Compare>,
                "bbtree supports stateless comparators only (PORTING.md, P8)");

  // Whether the comparator is the natural order, which inline keys encode.
  template <class K>
  static constexpr bool is_natural =
      std::is_same_v<Compare, std::less<K>> || std::is_same_v<Compare, std::less<>>;

  template <class A, class B>
  static bool less(const A& a, const B& b) noexcept {
    return Compare{}(a, b);
  }

  // Three ways in one comparison where `<=>` gives a weak order or stronger;
  // a key whose `<=>` is a partial order (`double`, or a pair that holds
  // one) compares by the comparator, twice, as std::map does.
  template <class A, class B>
  static std::weak_ordering cmp(const A& a, const B& b) noexcept {
    if constexpr ((std::is_same_v<Compare, std::less<A>> || std::is_same_v<Compare, std::less<>> ||
                   std::is_same_v<Compare, std::less<B>>) &&
                  std::three_way_comparable_with<A, B, std::weak_ordering>) {
      return std::weak_ordering(a <=> b);
    } else {
      if (Compare{}(a, b)) {
        return std::weak_ordering::less;
      }
      return Compare{}(b, a) ? std::weak_ordering::greater : std::weak_ordering::equivalent;
    }
  }
};

// The strategy a map was given, unwrapped: its inline keys and its node
// config, and what the shared map, file maps and stores add.
template <class S>
struct strategy_traits {
  using inline_keys = S;
  using config = default_node_config;
  using annex = void;
  static constexpr bool latched = false;
  static constexpr bool in_file = false;
};

template <class S, class C>
struct strategy_traits<configured<S, C>> : strategy_traits<S> {
  using config = C;
};

template <class S, class L>
struct strategy_traits<with_leaf_annex<S, L>> : strategy_traits<S> {
  using annex = L;
};

// Heap page size resolved: 0 in a config means the leaves'.
template <class C>
inline constexpr std::size_t heap_page_size_of =
    C::heap_page_size == 0 ? C::leaf_page_size : C::heap_page_size;

// Called around user code that runs while keys move (D48): an exception there
// would leave the move half done, so it ends the program, as the Rust crate's
// abort-on-unwind guard does.
struct while_keys_move {
  template <class F>
  static auto call(F&& user_code) noexcept {
    return std::forward<F>(user_code)();
  }
};

enum class inliner_kind : std::uint8_t {
  none,         // no inline keys
  heap_bytes,   // a std byte string that holds its bytes elsewhere
  array_bytes,  // a std byte array, which is its bytes
  trait,        // an inline_key specialization
};

// How keys `K` get inline keys under the map's comparator and strategy.
template <class K, class Compare, class InlineKeys>
struct key_inliner {
  static constexpr inliner_kind kind = [] {
    if constexpr (std::is_same_v<InlineKeys, plain_keys> || !key_order<Compare>::template is_natural<K>) {
      return inliner_kind::none;
    } else if constexpr (std_byte_string<K>) {
      return byte_string<K>::holds_its_bytes ? inliner_kind::array_bytes : inliner_kind::heap_bytes;
    } else if constexpr (has_inline_key<K>) {
      return inliner_kind::trait;
    } else {
      return inliner_kind::none;
    }
  }();

  static constexpr bool has_inline_keys = kind != inliner_kind::none;
  static constexpr bool is_byte_string =
      kind == inliner_kind::heap_bytes || kind == inliner_kind::array_bytes;

  // The width an `inline_key` specialization asks for, eight by default.
  static constexpr inline_width trait_width() {
    if constexpr (kind == inliner_kind::trait) {
      if constexpr (requires { inline_key<K>::width; }) {
        return inline_key<K>::width;
      }
    }
    return inline_width::eight;
  }
  static constexpr bool trait_has_hash_tag = [] {
    if constexpr (kind == inliner_kind::trait) {
      return requires(const K& key) { inline_key<K>::hash_tag(key); };
    } else {
      return false;
    }
  }();
  static constexpr bool trait_has_cmp_from = [] {
    if constexpr (kind == inliner_kind::trait) {
      return requires(const K& key) { inline_key<K>::cmp_from(key, key, std::size_t{}); };
    } else {
      return false;
    }
  }();
  static constexpr bool has_hash_tag = kind == inliner_kind::heap_bytes || trait_has_hash_tag;

  static std::span<const std::uint8_t> bytes(const K& key) noexcept
    requires(is_byte_string)
  {
    return byte_string<K>::bytes(key);
  }

  // The key's inline key at `offset`.
  static inline_word inline_key_at(const K& key, std::size_t offset) noexcept {
    if constexpr (is_byte_string) {
      return big_endian::window(bytes(key), offset);
    } else if constexpr (kind == inliner_kind::trait) {
      return inline_key<K>::at(key, offset);
    } else {
      return 0;
    }
  }

  // The key's hash tag (D36): a byte string's length, saturated at 255.
  static std::uint8_t hash_tag(const K& key) noexcept {
    if constexpr (kind == inliner_kind::heap_bytes || kind == inliner_kind::array_bytes) {
      const std::size_t len = bytes(key).size();
      return len < 255 ? static_cast<std::uint8_t>(len) : std::uint8_t{255};
    } else if constexpr (trait_has_hash_tag) {
      return while_keys_move::call([&] { return inline_key<K>::hash_tag(key); });
    } else {
      return 0;
    }
  }

  // How the key orders against `prefix`, the bytes every key shares (D50).
  static std::strong_ordering cmp_prefix(const K& key, std::span<const std::uint8_t> prefix) noexcept {
    if constexpr (kind == inliner_kind::trait) {
      if constexpr (requires { inline_key<K>::cmp_prefix(key, prefix); }) {
        return inline_key<K>::cmp_prefix(key, prefix);
      } else {
        std::size_t offset = 0;
        for (; offset + 8 <= prefix.size(); offset += 8) {
          const inline_word window = inline_key_at(key, offset);
          const inline_word shared = big_endian::load(prefix.data() + offset);
          if (window != shared) {
            return window <=> shared;
          }
        }
        const std::size_t tail = prefix.size() - offset;
        if (tail == 0) {
          return std::strong_ordering::equal;
        }
        const unsigned dropped_bits = static_cast<unsigned>(8 * (8 - tail));
        return (inline_key_at(key, offset) >> dropped_bits) <=>
               (big_endian::window(prefix, offset) >> dropped_bits);
      }
    } else {
      return search_bytes(bytes(key)).cmp_prefix(prefix);
    }
  }

  // How many leading bytes the zero-padded encodings of `a` and `b` share,
  // at most the longest prefix a node records. A node bounded by them may
  // skip that many bytes (05-fence-key.md).
  static std::size_t common_prefix_len(const K& a, const K& b) noexcept {
    constexpr std::size_t max_len = inline_key_node_header::max_prefix_len;
    if constexpr (!has_inline_keys) {
      return 0;
    } else {
      std::size_t limit = max_len;
      if constexpr (is_byte_string) {
        limit = std::max(bytes(a).size(), bytes(b).size());
      }
      for (std::size_t offset = 0; offset < limit; offset += 8) {
        const inline_word differing = inline_key_at(a, offset) ^ inline_key_at(b, offset);
        if (differing != 0) {
          return std::min(max_len, offset + static_cast<std::size_t>(std::countl_zero(differing)) / 8);
        }
      }
      return max_len;
    }
  }

  // How many leading bytes `key`'s encoding shares with `shared`, at most
  // shared.size().
  static std::size_t common_prefix_len_with_bytes(const K& key,
                                                  std::span<const std::uint8_t> shared) noexcept {
    if constexpr (!has_inline_keys) {
      return 0;
    } else if constexpr (is_byte_string) {
      const std::span<const std::uint8_t> key_bytes = bytes(key);
      const std::size_t both = std::min(key_bytes.size(), shared.size());
      std::size_t len = 0;
      while (len < both && key_bytes[len] == shared[len]) {
        ++len;
      }
      return len;
    } else {
      for (std::size_t offset = 0; offset < shared.size(); offset += 8) {
        const inline_word differing = inline_key_at(key, offset) ^ big_endian::window(shared, offset);
        if (differing != 0) {
          return std::min(shared.size(), offset + static_cast<std::size_t>(std::countl_zero(differing)) / 8);
        }
      }
      return shared.size();
    }
  }

  // Writes the first bytes of the key's encoding into `out`, at most
  // out.size(), and returns how many: a byte string's own, an integer's size,
  // another key's all of `out`, zero-padded.
  static std::size_t write_leading_bytes(const K& key, std::span<std::uint8_t> out) noexcept {
    if constexpr (!has_inline_keys) {
      return 0;
    } else {
      std::size_t len = out.size();
      if constexpr (is_byte_string) {
        len = std::min(len, bytes(key).size());
      } else if constexpr (std::is_integral_v<K> || std::is_same_v<K, int128> || std::is_same_v<K, uint128>) {
        len = std::min(len, sizeof(K));
      }
      for (std::size_t offset = 0; offset < len; offset += 8) {
        const inline_word window = inline_key_at(key, offset);
        for (std::size_t byte = 0; byte < 8 && offset + byte < len; ++byte) {
          out[offset + byte] = static_cast<std::uint8_t>(window >> (56 - 8 * byte));
        }
      }
      return len;
    }
  }
};

// How a search runs over keys by comparing them (D19).
enum class key_search : std::uint8_t {
  // Binary search with no early exit, each step a conditional move.
  branchless,
  // Binary search that branches on each comparison and stops at an equal key.
  early_exit,
};

// What the nodes of one kind store beside their keys, and how a search tells
// their keys apart (D47): a row of 07-node-plan.md's table, for one kind.
struct node_plan {
  inliner_kind inliner;
  // Bytes of each inline key the node stores; none without inline keys.
  std::optional<inline_width> width;
  // Whether the node stores fewer bytes than a leaf, so a window it hands out
  // is read from its key again (D43).
  bool rehydrates_windows;
  // Whether the node stores each key's hash tag (D36).
  bool hash_tags;
  // The search over the keys whose inline keys tie with the search key's.
  key_search candidate_search;
  // The search over all keys, when no inline key decides (D19).
  key_search all_keys_search;
  // Whether an exact-key lookup confirms its one candidate by comparing all
  // of its bytes at once (D38).
  bool whole_key_equality;

  constexpr bool has_inline_keys() const { return width.has_value(); }
  constexpr bool is_for_heap_byte_strings() const { return inliner == inliner_kind::heap_bytes; }
  // Whether every lookup's node search is inlined: all but heap byte
  // strings', whose searches read past the node anyway (D41).
  constexpr bool inlines_node_search() const { return !is_for_heap_byte_strings(); }

  template <class K>
  static constexpr key_search search_for_key() {
    return std::is_trivially_destructible_v<K> && sizeof(K) <= 16 ? key_search::branchless
                                                                  : key_search::early_exit;
  }

  // The plan of one node kind for keys `K` with inliner `I`. `hash_tags` says
  // whether the plan wants them; the layout decides whether they fit
  // (`tree_types`).
  template <class K, class I>
  static constexpr node_plan of(bool is_leaf) {
    node_plan plan{I::kind, std::nullopt, false, false, search_for_key<K>(), search_for_key<K>(), false};
    switch (I::kind) {
      case inliner_kind::none:
        break;
      case inliner_kind::trait: {
        // Keys with cmp_from compare ties past the bytes known equal, so
        // internal nodes keep 4 bytes of 8-byte inline keys (D43).
        const bool narrower_inside =
            I::trait_has_cmp_from && I::trait_width() == inline_width::eight && !is_leaf;
        plan.width = narrower_inside ? inline_width::four : I::trait_width();
        plan.rehydrates_windows = narrower_inside;
        plan.hash_tags = I::trait_has_hash_tag && is_leaf;
        break;
      }
      case inliner_kind::array_bytes:
        // Bytes in the node: a tie is a load, so 2 bytes per inline key (D32).
        plan.width = inline_width::two;
        plan.candidate_search = key_search::early_exit;
        plan.whole_key_equality = true;
        break;
      case inliner_kind::heap_bytes:
        // Ties read past the node: 8 bytes in leaves, 4 inside (D43).
        plan.width = is_leaf ? inline_width::eight : inline_width::four;
        plan.rehydrates_windows = !is_leaf;
        plan.hash_tags = is_leaf;
        plan.candidate_search = key_search::early_exit;
        break;
    }
    return plan;
  }
};

// An inline key with the prefix length it is taken at: bytes
// prefix_len..prefix_len + 8 of a key's encoding (05-fence-key.md).
class window {
 public:
  constexpr window() noexcept = default;
  constexpr window(std::size_t prefix_len, inline_word inline_key) noexcept
      : prefix_len_(prefix_len), inline_key_(inline_key) {}

  constexpr std::size_t prefix_len() const noexcept { return prefix_len_; }
  constexpr inline_word inline_key() const noexcept { return inline_key_; }

  // The same key's window at `fill`'s shorter prefix length, where `fill` is
  // the window there of a key that shares this key's bytes up to this
  // window's prefix length. Reads no key: how a node's prefix shrinks.
  constexpr window moved_back_to(window fill) const noexcept {
    const std::size_t shift = prefix_len_ > fill.prefix_len_ ? prefix_len_ - fill.prefix_len_ : 0;
    inline_word key = inline_key_;
    if (shift >= 8) {
      key = fill.inline_key_;
    } else if (shift > 0) {
      const unsigned shift_bits = static_cast<unsigned>(8 * shift);
      const inline_word filled_bits = ~(~inline_word{0} >> shift_bits);
      key = (fill.inline_key_ & filled_bits) | (inline_key_ >> shift_bits);
    }
    return {fill.prefix_len_, key};
  }

  friend constexpr bool operator==(window, window) = default;

 private:
  std::size_t prefix_len_ = 0;
  inline_word inline_key_ = 0;
};

// A key outside the nodes, with its window for the node it goes into; the
// window is empty in a map without inline keys.
template <class K>
struct windowed_key {
  K key;
  bbtree::detail::window window;
};

}  // namespace bbtree::detail
