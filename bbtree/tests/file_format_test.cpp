// The file map's format (D58), after the Rust crate's tests of `tree/file.rs`:
// what a checked open refuses, a field at a time, so that no bytes lead a
// reader out of them. Each case damages a valid file map in one place
// (`file_image`) and names the refusal.
#include "../fuzz/file_image.hpp"

#include <bbtree/file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using u64_image = bbtree_fuzz::file_image<std::uint64_t, std::uint64_t>;

// The file map of `count` entries of `u64` keys and values, `key → ~key`:
// 5000 take a root and two levels below it.
u64_image u64_image_of(std::uint64_t count) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  for (std::uint64_t key = 0; key < count; ++key) {
    entries.emplace_back(key, ~key);
  }
  using writer = bbtree::file_writer<std::uint64_t, std::uint64_t>;
  return u64_image(writer::from_ascending(entries.begin(), entries.end()).to_buf().bytes());
}

}  // namespace

TEST_CASE("edges that lead back, outside or twice are refused", "[file]") {
  const u64_image image = u64_image_of(5000);
  const bbtree_fuzz::image_node root = image.nodes().front();
  REQUIRE_FALSE(root.is_leaf);
  const auto edge = [&](const bbtree_fuzz::image_node& node, std::size_t index) {
    return image.get<std::uint64_t>(u64_image::edge_slot(node, index));
  };
  const auto with_edge = [&](const bbtree_fuzz::image_node& node, std::size_t index, std::uint64_t tagged) {
    return image.corruption_after(
        [&](u64_image& damaged) { damaged.put(u64_image::edge_slot(node, index), tagged); });
  };
  const std::string leads_back = "an edge that does not lead forward";
  CHECK(with_edge(root, 0, 0) == leads_back);
  CHECK(with_edge(root, 1, edge(root, 0)) == leads_back);
  CHECK(with_edge(root, 0, ~std::uint64_t{0} - 1) == leads_back);
  CHECK(with_edge(root, 0, edge(root, 0) | 1) == "children of both kinds at one depth");

  // The edge to the last leaf, which no page is reached after.
  const bbtree_fuzz::image_node last_parent = image.last_internal_at(1);
  const std::size_t to_last_leaf = image.len(last_parent);
  const std::string outside = "a page outside the file map, or misaligned";
  const std::uint64_t past_the_end = image.bytes().size() - last_parent.offset;
  CHECK(with_edge(last_parent, to_last_leaf, past_the_end | 1) == outside);
  CHECK(with_edge(last_parent, to_last_leaf, edge(last_parent, to_last_leaf) + 8) == outside);
  // Pages so far out that their ends wrap around the address space: the
  // bytes before the file map are not the file map's.
  for (const std::uint64_t pages_before : {1u, 2u, 4096u}) {
    const std::uint64_t before = 0 - pages_before * u64_image::types::leaf.page.size;
    CHECK(with_edge(last_parent, to_last_leaf, (before - last_parent.offset) | 1) == outside);
  }
}

TEST_CASE("a root page outside the file map is refused, checked or not", "[file]") {
  for (const std::uint64_t count : {10u, 5000u}) {
    const u64_image image = u64_image_of(count);
    const std::uint64_t tag = image.get<std::uint64_t>(u64_image::root_slot) & 1;
    const std::uint64_t page_size = u64_image::types::root_page.size;
    const std::uint64_t size = image.bytes().size();
    // Before the file map, its end wrapped; past its end; in its header; and
    // not at a page's alignment.
    for (const std::uint64_t root :
         {0 - page_size, 0 - 2 * page_size, size, size - page_size + 8, std::uint64_t{64},
          std::uint64_t{u64_image::types::root_page.align} / 2}) {
      u64_image damaged = image;
      damaged.put(u64_image::root_slot, root | tag);
      CHECK(damaged.corruption() == "a root page outside the file map");
      CHECK(damaged.corruption_unchecked() == "a root page outside the file map");
    }
  }
}

TEST_CASE("a leaf below the root without keys is refused", "[file]") {
  const u64_image image = u64_image_of(5000);
  const std::vector<bbtree_fuzz::image_node> leaves = image.leaves();
  // The first leaf, one inside, and the last: a walk would step onto each
  // one's first key from the separator before it, or onto its last from the
  // one after. The header's count follows the leaf's, so that it is not the
  // count that refuses the file map.
  for (const std::size_t index : {std::size_t{0}, std::size_t{2}, leaves.size() - 1}) {
    CHECK(image.corruption_after([&](u64_image& damaged) { damaged.resize_leaf(leaves[index], 0); }) ==
          "a leaf below the root without keys");
  }
}

TEST_CASE("a leaf's head flag that its place contradicts is refused", "[file]") {
  const std::string misplaced = "a head in the first leaf, or none in a later one";
  for (const std::uint64_t count : {40u, 5000u}) {
    const u64_image image = u64_image_of(count);
    const std::vector<bbtree_fuzz::image_node> leaves = image.leaves();
    const auto with_flag = [&](std::size_t index, std::uint8_t flag) {
      return image.corruption_after([&](u64_image& damaged) { damaged.set_head_flag(leaves[index], flag); });
    };
    CHECK(with_flag(0, 1) == misplaced);
    // A reader takes a separator's key and value from the leaf right of it
    // without asking the leaf: it has them.
    for (std::size_t index = 1; index < leaves.size(); index += leaves.size() / 3 + 1) {
      CHECK(with_flag(index, 0) == misplaced);
    }
    CHECK(with_flag(leaves.size() - 1, 2) == "a node header out of range");
  }
}

TEST_CASE("an internal node's edges past its last are zeros", "[file]") {
  // The builder raises the root twice on the way to 5000 entries, and moves
  // edges to stock its right border: the pages keep the pointers they held.
  const u64_image image = u64_image_of(5000);
  std::size_t unused = 0;
  for (const bbtree_fuzz::image_node& node : image.nodes()) {
    if (node.is_leaf) {
      continue;
    }
    for (std::size_t edge = image.len(node) + 1; edge <= u64_image::types::internal_capacity; ++edge) {
      CHECK(image.get<std::uint64_t>(u64_image::edge_slot(node, edge)) == 0);
      ++unused;
    }
  }
  CHECK(unused > 0);
}
