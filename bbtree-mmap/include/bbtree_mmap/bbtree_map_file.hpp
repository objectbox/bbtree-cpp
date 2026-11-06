// `bbtree_mmap::bbtree_map_file`: a bbtree map compacted into a file
// (bbtree's file maps, D58), mapped back into memory or read into it, and
// looked up where it lies, with the map's own code, at a map's speed.
//
//   bbtree::unsync::bbtree_map<std::uint64_t, std::string> names{{7, "ferris"}, {42, "corro"}};
//   bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::create("names.bbtree", names);
//   auto file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::mmap("names.bbtree");
//   file.get(42);  // std::optional<std::string_view>{"corro"}
//
// Each key and value type keeps its own storage (`disk_type`): plain types as
// they are, in their slots; `std::string` and `std::vector<std::uint8_t>` as
// spans to bytes in heap pages after their leaves (`var_bytes.hpp`).
#pragma once

#include <bbtree/file.hpp>
#include <bbtree/unsync/map.hpp>
#include <bbtree_mmap/heap_pages.hpp>
#include <bbtree_mmap/mapped_file.hpp>
#include <bbtree_mmap/var_bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bbtree_mmap {

// How a type is kept in a map file: what its slot stores, what a writer's tree
// holds, what a reader hands out, and what a lookup compares.
template <class T>
struct disk_type;

template <bbtree::plain_bytes T>
struct disk_type<T> {
  using stored = T;
  using in_writer = T;
  using view = T;
  static in_writer to_writer(const T& value, byte_arena&) { return value; }
  static view view_of(const stored& slot) noexcept { return slot; }
  template <class F>
  static decltype(auto) with_query(const T& key, F&& look_up) {
    return look_up(key);
  }
};

template <class Owned, class View>
struct var_disk_type {
  using stored = var_bytes;
  using in_writer = arena_bytes;
  using view = View;
  static in_writer to_writer(const Owned& value, byte_arena& arena) {
    return arena.add(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
  }
  static view view_of(const stored& slot) noexcept {
    const auto bytes = slot.bytes();
    return view(reinterpret_cast<const typename View::value_type*>(bytes.data()), bytes.size());
  }
  template <class F>
  static decltype(auto) with_query(View key, F&& look_up) {
    const var_bytes_query query(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
    return look_up(query.span());
  }
};

template <>
struct disk_type<std::string> : var_disk_type<std::string, std::string_view> {};
template <>
struct disk_type<std::vector<std::uint8_t>>
    : var_disk_type<std::vector<std::uint8_t>, std::span<const std::uint8_t>> {};

template <class K, class V>
class bbtree_map_file {
  using stored_key = typename disk_type<K>::stored;
  using stored_value = typename disk_type<V>::stored;
  using writer =
      bbtree::file_writer<stored_key, stored_value, std::less<>, bbtree::auto_inline_keys, heap_pages>;
  using reader = bbtree::file_map<stored_key, stored_value, std::less<>, bbtree::auto_inline_keys>;

 public:
  using key_view = typename disk_type<K>::view;
  using value_view = typename disk_type<V>::view;

  // Writes `map` to a new file at `path`.
  template <class Compare, class A, class S>
  static void create(const std::filesystem::path& path,
                     const bbtree::unsync::bbtree_map<K, V, Compare, A, S>& map) {
    const bbtree::file_buf bytes = bytes_of(map);
    mapped_file::write(path, bytes.bytes());
  }

  // The file map of `map`, in memory.
  template <class Compare, class A, class S>
  static bbtree::file_buf bytes_of(const bbtree::unsync::bbtree_map<K, V, Compare, A, S>& map) {
    byte_arena arena;
    std::vector<std::pair<typename disk_type<K>::in_writer, typename disk_type<V>::in_writer>> entries;
    entries.reserve(map.size());
    for (const auto& [key, value] : map) {
      entries.emplace_back(disk_type<K>::to_writer(key, arena), disk_type<V>::to_writer(value, arena));
    }
    return writer::from_ascending(entries.begin(), entries.end()).to_buf();
  }

  // Maps the file at `path`, checking every page; nothing may change the
  // file while it is mapped.
  static bbtree_map_file mmap(const std::filesystem::path& path, os_pages pages = os_pages::base) {
    return bbtree_map_file(mapped_file::map(path, pages), true);
  }

  // Maps the file without checking its pages: for a trusted file.
  static bbtree_map_file mmap_unchecked(const std::filesystem::path& path, os_pages pages = os_pages::base) {
    return bbtree_map_file(mapped_file::map(path, pages), false);
  }

  // Reads the file at `path` into memory, checking every page.
  static bbtree_map_file read(const std::filesystem::path& path) {
    return bbtree_map_file(mapped_file::read(path, reader::alignment()), true);
  }

  std::size_t size() const noexcept { return map_.size(); }
  bool empty() const noexcept { return map_.empty(); }

  template <class Q>
  std::optional<value_view> get(const Q& key) const {
    return disk_type<K>::with_query(key, [&](const stored_key& query) -> std::optional<value_view> {
      const stored_value* value = map_.get(query);
      if (value == nullptr) {
        return std::nullopt;
      }
      return disk_type<V>::view_of(*value);
    });
  }

  template <class Q>
  bool contains(const Q& key) const {
    return disk_type<K>::with_query(key, [&](const stored_key& query) { return map_.contains(query); });
  }

  // Runs `f(key, value)` on every entry in key order.
  template <class F>
  void for_each(F&& f) const {
    for (const auto& entry : map_) {
      f(disk_type<K>::view_of(entry.first), disk_type<V>::view_of(entry.second));
    }
  }

 private:
  bbtree_map_file(mapped_file&& file, bool checked)
      : file_(std::move(file)),
        map_(checked ? reader::open(file_.bytes()) : reader::open_unchecked(file_.bytes())) {}

  mapped_file file_;
  reader map_;
};

}  // namespace bbtree_mmap
