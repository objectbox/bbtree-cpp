// The maps and sets of tracked entries that the drop-accounting and failure
// tests run on: every way the nodes hold and move a key or a value, in at
// least two mixes, on a checking allocator.
#pragma once

#include "checked_pages.hpp"
#include "instrumented.hpp"

#include <bbtree/unsync/map.hpp>
#include <bbtree/unsync/set.hpp>

#include <functional>

namespace bbtree_test {

template <class K, class V>
using checked_map = bbtree::unsync::bbtree_map<K, V, std::less<K>, checked_allocator>;
template <class K>
using checked_set = bbtree::unsync::bbtree_set<K, std::less<K>, checked_allocator>;

// Keys without inline keys, which internal nodes keep; every move is one
// object at a time.
using singly_moved_plain_keys =
    checked_map<tracked_key<moves::one_at_a_time, key_window::none>, tracked_value<moves::one_at_a_time>>;
// Separators' keys in leaf heads (D49), beside values that move as bytes.
using singly_moved_inline_keys_byte_moved_values =
    checked_map<tracked_key<moves::one_at_a_time, key_window::eight_bytes>, tracked_value<moves::as_bytes>>;
// Plain keys and values that move as bytes.
using byte_moved_plain_keys =
    checked_map<tracked_key<moves::as_bytes, key_window::none>, tracked_value<moves::as_bytes>>;
// Two-byte inline keys, as the std byte arrays' nodes keep.
using byte_moved_narrow_inline_keys_singly_moved_values =
    checked_map<tracked_key<moves::as_bytes, key_window::two_bytes>, tracked_value<moves::one_at_a_time>>;
// A leaf shifts whole entries, key and value, in one copy (D29).
using byte_moved_paired_entries =
    checked_map<tracked_key<moves::as_bytes, key_window::eight_bytes>, tracked_value<moves::as_bytes>>;

// Entries that can't be copied, as every Rust test type.
using move_only_singly_moved_inline_keys =
    checked_map<tracked_key<moves::one_at_a_time, key_window::eight_bytes, copies::deleted>,
                tracked_value<moves::as_bytes, copies::deleted>>;
using move_only_byte_moved_plain_keys =
    checked_map<tracked_key<moves::as_bytes, key_window::none, copies::deleted>,
                tracked_value<moves::one_at_a_time, copies::deleted>>;

using singly_moved_plain_key_set = checked_set<tracked_key<moves::one_at_a_time, key_window::none>>;
using singly_moved_inline_key_set = checked_set<tracked_key<moves::one_at_a_time, key_window::eight_bytes>>;
using byte_moved_plain_key_set = checked_set<tracked_key<moves::as_bytes, key_window::none>>;
using byte_moved_narrow_inline_key_set = checked_set<tracked_key<moves::as_bytes, key_window::two_bytes>>;

}  // namespace bbtree_test

// The type lists of the template test cases.
#define BBTREE_TEST_COPYABLE_TRACKED_MAPS                                                                 \
  bbtree_test::singly_moved_plain_keys, bbtree_test::singly_moved_inline_keys_byte_moved_values,          \
      bbtree_test::byte_moved_plain_keys, bbtree_test::byte_moved_narrow_inline_keys_singly_moved_values, \
      bbtree_test::byte_moved_paired_entries
#define BBTREE_TEST_TRACKED_MAPS                                                      \
  BBTREE_TEST_COPYABLE_TRACKED_MAPS, bbtree_test::move_only_singly_moved_inline_keys, \
      bbtree_test::move_only_byte_moved_plain_keys
#define BBTREE_TEST_TRACKED_SETS                                                     \
  bbtree_test::singly_moved_plain_key_set, bbtree_test::singly_moved_inline_key_set, \
      bbtree_test::byte_moved_plain_key_set, bbtree_test::byte_moved_narrow_inline_key_set
