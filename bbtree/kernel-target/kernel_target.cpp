// bbtree's maps as code without vector registers builds them (the Rust
// crate's `kernel-target`, x86_64-unknown-none): compiled with `__SSE2__`
// and `__POPCNT__` undefined, so the inline-key count takes its binary search
// and no intrinsic is used (inline_key_search.hpp), and run. A true
// freestanding build would need a freestanding standard library: libstdc++'s
// headers need `long double`, which `-mgeneral-regs-only` rules out
// (PORTING.md, P14).
#include <bbtree/unsync/map.hpp>

#include <cstdint>

int main() {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t key = 0; key < 20'000; ++key) {
    map.insert({key * 7 % 20'011, key});
  }
  for (std::uint64_t key = 0; key < 20'000; key += 3) {
    map.erase(key * 7 % 20'011);
  }
  std::uint64_t count = 0;
  std::uint64_t previous = 0;
  bool ascending = true;
  for (const auto& entry : map) {
    ascending = ascending && (count == 0 || entry.first > previous);
    previous = entry.first;
    ++count;
  }
  map.check_invariants();
  return ascending && count == map.size() && map.size() == 20'000 - 6667 ? 0 : 1;
}
