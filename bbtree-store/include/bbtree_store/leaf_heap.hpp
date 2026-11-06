// `leaf_heap`: each leaf's annex in a store's map (D60, bbtree's
// `leaf_annex`): the chain of heap pages its records go into
// (`heap_page.hpp`), and its generation. `record_heap` runs what a store does
// to a leaf's records, while the map holds the leaf exclusively.
//
// - A record goes after the last one in the leaf's newest page, the chain's
//   head; a page goes in front when the head lacks room.
// - A split moves no record: the new leaf starts with no page, and its
//   records lie in the old leaf's chain. Both leaves take the next
//   generation.
// - A merge splices the leaving leaf's chain in behind the head.
// - A record that dies leaves a hole. Once a page of the leaf's own chain is
//   down to half its bytes live, and the chain too, or a record finds no room
//   in a chain a quarter holes, the leaf compacts, in place: in each page
//   where only its own records live, it slides them together; it frees the
//   pages where none of its records live, or orphans them while other leaves'
//   do. A page where other leaves' records live keeps its holes until they
//   go. Compacting takes no page, so a store whose allocator refuses pages
//   still takes its holes back.
// - A record two generations behind its leaf that lies in another leaf's
//   chain moves into its own leaf's head, once a generation: the leaf's key
//   range split twice since it was written, so its page holds the records of
//   leaves further and further off. One generation behind, it shares a page
//   with a neighbour's.
// - A value too long for its record goes into overflow pages
//   (`overflow.hpp`), written before its leaf is held, which its record
//   links: a record that moves takes the link along, and one that dies gives
//   the pages back.
// - A leaf's head key's record stays put (PORTING.md, P24): the head is the
//   separator left of the leaf, which a thread holding an internal node reads
//   without holding the leaf. No compaction or pull moves it, and a value too
//   long for it goes into a new record that it becomes a stub of, in place
//   (`record.hpp`); the stub's target moves as any record does.
//
// Only a thread holding a leaf exclusively moves, changes or kills its
// records, and reads or changes its chain: a record's own leaf, that is, the
// leaf of the key that points to it, which may lie in another leaf's chain.
// Pages hold a count of their live bytes for that (`heap_page.hpp`).
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/leaf_annex.hpp>
#include <bbtree/map.hpp>
#include <bbtree/node_config.hpp>
#include <bbtree_store/heap_page.hpp>
#include <bbtree_store/overflow.hpp>
#include <bbtree_store/record.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace bbtree_store::detail {

// How much of a chain's pages records took, and how much of that is live.
struct chain_use {
  std::size_t used = 0;
  std::size_t live = 0;

  // Whether at least half of what records took is holes.
  bool is_half_holes() const noexcept { return used > 0 && live * 2 <= used; }

  // Whether at least a quarter of what records took is holes: worth a
  // compaction, which moves at most three live bytes a byte of holes, to save
  // a page.
  bool is_quarter_holes() const noexcept { return used > 0 && live * 4 <= used * 3; }
};

// A leaf's chain of heap pages, and its generation: the annex of a store's
// map's leaves.
template <class C>
class leaf_heap {
 public:
  using page = heap_page<C>;

  // The leaf split: both halves take the next generation; the new one starts
  // with no page.
  void split(leaf_heap& right) noexcept {
    generation_ = generation_count::next(generation_);
    right.generation_ = generation_;
    right.pulled_ = pulled_;
  }

  // `right`'s leaf merged into this one: its chain goes in behind the head.
  void merge(leaf_heap&& right) noexcept {
    if (!right.head_) {
      return;
    }
    if (!head_) {
      head_ = std::exchange(right.head_, page());
      return;
    }
    page right_tail = right.head_;
    while (right_tail.next()) {
      right_tail = right_tail.next();
    }
    right_tail.set_next(head_.next());
    head_.set_next(std::exchange(right.head_, page()));
  }

  // The leaf goes with the map: no thread reads a record again, and each page
  // is in this chain alone.
  template <class A>
  void release(const A& allocator) && noexcept {
    for (page at = std::exchange(head_, page()); at;) {
      const page next = at.next();
      at.deallocate(allocator);
      at = next;
    }
  }

  // The chain's newest page, where records go; none without one.
  page head() const noexcept { return head_; }
  void set_head(page head) noexcept { head_ = head; }

  // How many times the leaf split, with the leaves it split from, wrapping.
  std::uint8_t generation() const noexcept { return generation_; }
  // The generation the leaf last pulled its records in at.
  std::uint8_t pulled() const noexcept { return pulled_; }
  void set_pulled(std::uint8_t generation) noexcept { pulled_ = generation; }

  // `size` bytes of room, live, in the chain's head page; else in a page of
  // the chain that has them, which becomes the head; null if none has.
  std::byte* room(std::size_t size) noexcept {
    page before;
    for (page at = head_; at; at = at.next()) {
      if (at.room() >= size) {
        if (before) {
          before.set_next(at.next());
          at.set_next(head_);
          head_ = at;
        }
        return at.append(size);
      }
      before = at;
    }
    return nullptr;
  }

  // `size` bytes of room, live, in a new head page from `allocator`; null if
  // it refuses.
  template <class A>
  std::byte* room_in_new_page(std::size_t size, const A& allocator) noexcept {
    const page fresh = page::allocate(allocator);
    if (!fresh) {
      return nullptr;
    }
    fresh.set_next(head_);
    head_ = fresh;
    return fresh.append(size);
  }

  // How much of the chain records took, and how much of that is live.
  chain_use use() const noexcept {
    chain_use counted;
    for (page at = head_; at; at = at.next()) {
      counted.used += at.used();
      counted.live += at.live();
    }
    return counted;
  }

  // Whether the chain holds `wanted`.
  bool holds(page wanted) const noexcept {
    for (page at = head_; at; at = at.next()) {
      if (at == wanted) {
        return true;
      }
    }
    return false;
  }

 private:
  page head_;
  std::uint8_t generation_ = 0;
  std::uint8_t pulled_ = 0;
};

// The map under a store of allocator `A` and node config `C`: keys pointing
// to their records, no values, byte strings' inline keys, and a `leaf_heap`
// in each leaf.
template <class A, class C>
using store_map = bbtree::bbtree_map<
    store_key, no_value, std::less<store_key>, A,
    bbtree::with_leaf_annex<bbtree::configured<bbtree::auto_inline_keys, C>, leaf_heap<C>>>;

// Where a store's records go: its leaves' heaps, from its allocator.
template <class A, class C>
class record_heap {
  using page = heap_page<C>;
  using store_leaf = typename store_map<A, C>::annexed_leaf_type;

 public:
  explicit record_heap(const A& allocator) noexcept : allocator_(allocator) {}
  record_heap(const record_heap&) = delete;
  record_heap& operator=(const record_heap&) = delete;

  // Whether dropping the store must kill its records, for pages outside the
  // chains to go back: orphans that hold live records, or overflow pages,
  // once any value took some.
  bool kills_at_drop() const noexcept {
    return orphans_.load(std::memory_order_acquire) > 0 || overflowed_.load(std::memory_order_relaxed);
  }

  // The overflow pages of the record of a key of `key_len` and `value`,
  // written, for `write`: the value's bytes the record doesn't keep; none if
  // it keeps them all. Throws `alloc_error` if a page is refused.
  overflow<C> write_overflow(std::size_t key_len, bytes value) {
    const std::size_t kept = record<C>::kept_len(key_len, value.size());
    if (kept == value.size()) {
      return overflow<C>();
    }
    const overflow<C> pages = overflow<C>::write(value.subspan(kept), allocator_);
    if (!pages) {
      throw bbtree::alloc_error();
    }
    if (!overflowed_.load(std::memory_order_relaxed)) {
      overflowed_.store(true, std::memory_order_relaxed);
    }
    return pages;
  }

  // Gives back the overflow pages of a record that never went in.
  void free_overflow(overflow<C> pages) const noexcept { pages.free(allocator_); }

  // Writes the record of `key` and `value`, whose overflow pages
  // `write_overflow` wrote, into `leaf`'s heap, for a key of `leaf` to point
  // to; throws `alloc_error` if a page it needs is refused. The leaf's
  // records may move first, for room, or to their own leaf (top of the file).
  record<C> write(store_leaf& leaf, bytes key, bytes value, overflow<C> pages) {
    pull_stale(leaf);
    std::byte* at = room_for(leaf, record<C>::size_of(key.size(), value.size()));
    return record<C>::write(at, key, value, pages, leaf.annex().generation());
  }

  // Gives `stored`, a key of `leaf`, whose bytes are `key`, `value` in place
  // of its own: in its record if it keeps its value whole and the new one is
  // no longer, else in a new record (`write`). The leaf's head keeps its
  // record, which becomes, or stays, a stub of the new one (top of the file).
  void overwrite(store_leaf& leaf, const store_key& stored, bytes key, bytes value, overflow<C> pages) {
    const record<C> old = stored.stored_record<C>();
    const bool pinned = leaf.is_head(stored);
    const record<C> holder = old.is_stub() ? old.target() : old;
    const record_value<C> old_value = holder.value();
    if (!old_value.has_overflow() && value.size() <= old_value.len()) {
      const std::size_t size = holder.size();
      holder.overwrite_value(value);
      (void)kill_in(page::of_record(holder.address()), size - holder.size());
      if (old.is_stub()) {
        old.retarget(holder);
      }
      return;
    }
    const record<C> fresh = write(leaf, key, value, pages);
    if (!pinned) {
      kill(leaf, stored.replace(fresh));
      return;
    }
    // The write may have moved the stub's target: read it again.
    const record<C> current = stored.stored_record<C>();
    if (current.is_stub()) {
      const record<C> replaced = current.target();
      current.retarget(fresh);
      kill(leaf, replaced);
      return;
    }
    const std::size_t size = current.size();
    current.overflow_pages().free(allocator_);
    current.become_stub_of(fresh);
    (void)kill_in(page::of_record(current.address()), size - current.size());
  }

  // `removed`, a key of `leaf`, is about to go. If its record lies in the
  // leaf's chain, and the page, and the chain, are half holes once it dies,
  // the leaf compacts, the record left as it is: the map reads the key until
  // the removal returns, and the record dies only then (`kill_record`). Its
  // page counts it live meanwhile, so no compaction slides over it or frees
  // it.
  void before_removal(store_leaf& leaf, const store_key& removed) {
    const record<C> doomed = removed.stored_record<C>();
    const page in = page::of_record(doomed.address());
    const std::size_t size = doomed.size();
    if (!leaf.annex().holds(in) || (in.live() - size) * 2 > in.used()) {
      return;
    }
    chain_use after = leaf.annex().use();
    after.live -= size;
    if (after.is_half_holes()) {
      compact(leaf, &removed);
    }
  }

  // `doomed` dies, which no key the store reads again points to, and no
  // thread reads: of a key whose removal returned, or of a store that drops.
  // Its overflow pages go back, and its bytes die in its page, which may be
  // any leaf's then, or an orphan; whether that was an orphan they emptied,
  // which went back too.
  bool kill_record(record<C> doomed) noexcept {
    if (doomed.is_stub()) {
      // A stub's target is its own: it dies too.
      (void)kill_record(doomed.target());
    } else {
      doomed.overflow_pages().free(allocator_);
    }
    return kill_in(page::of_record(doomed.address()), doomed.size());
  }

 private:
  // `doomed`, of a key of `leaf` that points elsewhere now, dies
  // (`kill_record`). If it lay in the leaf's own chain, the leaf compacts
  // once half the page, and half the chain, are holes.
  void kill(store_leaf& leaf, record<C> doomed) {
    const page in = page::of_record(doomed.address());
    // Before the kill: once it is done, another leaf's page may go.
    const bool own = leaf.annex().holds(in);
    if (kill_record(doomed) || !own) {
      return;
    }
    if (in.live() * 2 <= in.used() && leaf.annex().use().is_half_holes()) {
      compact(leaf, nullptr);
    }
  }

  // `size` bytes of `in`'s records died; whether it was an orphan they
  // emptied, which goes back to the allocator then.
  bool kill_in(page in, std::size_t size) noexcept {
    if (!in.kill(size)) {
      return false;
    }
    orphans_.fetch_sub(1, std::memory_order_acq_rel);
    // An orphan that a kill emptied is that kill's alone.
    in.deallocate(allocator_);
    return true;
  }

  // `size` bytes of room in `leaf`'s chain, live: in its head page, or
  // another page with room, which becomes the head; else, once a compaction
  // slid the records together if a quarter of the chain is holes, there;
  // else in a new head page; else, if the allocator has none to give, once a
  // compaction took back whatever holes there are. Throws `alloc_error`
  // without room.
  std::byte* room_for(store_leaf& leaf, std::size_t size) {
    if (std::byte* at = leaf.annex().room(size)) {
      return at;
    }
    if (leaf.annex().use().is_quarter_holes()) {
      compact(leaf, nullptr);
      if (std::byte* at = leaf.annex().room(size)) {
        return at;
      }
    }
    if (std::byte* at = leaf.annex().room_in_new_page(size, allocator_)) {
      return at;
    }
    const chain_use chain = leaf.annex().use();
    if (chain.live < chain.used) {
      compact(leaf, nullptr);
    }
    if (std::byte* at = leaf.annex().room(size)) {
      return at;
    }
    throw bbtree::alloc_error();
  }

  // Slides the records of `leaf`'s keys, but `gone`'s, which stays where it
  // is, together in the pages of its chain where no other leaf's record
  // lives, in place, as one run from the first such page on; frees those
  // pages it empties, and the pages where none of its records live, or
  // orphans them while other leaves' do (`heap_page.hpp`). The page the run
  // ends in becomes the head. Then the leaf's records in other leaves' pages
  // move in, where there is room, or pages to be had: those pages' leaves can
  // slide them then, and orphans empty.
  //
  // A page's live bytes that its leaf's records don't account for are other
  // leaves': a page whose live bytes they all account for has no other live
  // record, and gets none, as only its leaf writes it.
  void compact(store_leaf& leaf, const store_key* gone) {
    // Every allocation first: once a page is let go, nothing may throw.
    std::vector<page> chain;
    for (page at = leaf.annex().head(); at; at = at.next()) {
      chain.push_back(at);
    }
    const auto in_chain = [&](const std::byte* address) {
      return std::find(chain.begin(), chain.end(), page::of_record(address)) != chain.end();
    };
    // The leaf's records in its chain, by address: each page's together. The
    // head's record stays put (top of the file): to the compaction it is
    // another leaf's. A stub's target moves, its stub pointed to it again.
    struct own_record {
      std::byte* address;
      const store_key* key;
      bool is_target;
    };
    std::vector<own_record> own;
    for (std::size_t index = 0; index < leaf.key_count(); ++index) {
      const store_key& key = leaf.key(index);
      if (&key == gone) {
        continue;
      }
      const record<C> stored = key.stored_record<C>();
      if (!leaf.is_head(key) && in_chain(stored.address())) {
        own.push_back({stored.address(), &key, false});
      }
      if (stored.is_stub() && in_chain(stored.target().address())) {
        own.push_back({stored.target().address(), &key, true});
      }
    }
    std::sort(own.begin(), own.end(),
              [](const own_record& a, const own_record& b) { return std::less<>()(a.address, b.address); });
    const auto records_in = [&](page in) {
      const auto first = std::partition_point(own.begin(), own.end(), [&](const own_record& r) {
        return std::less<>()(r.address, in.records_start());
      });
      const auto last =
          std::find_if(first, own.end(), [&](const own_record& r) { return !in.holds(r.address); });
      return std::pair(first, last);
    };
    // The pages the run slides over, and those others' records keep.
    std::vector<page> run;
    std::vector<page> kept;
    run.reserve(chain.size());
    kept.reserve(chain.size());
    for (const page at : chain) {
      const auto [first, last] = records_in(at);
      std::size_t own_bytes = 0;
      for (auto r = first; r != last; ++r) {
        own_bytes += record<C>(r->address).size();
      }
      if (own_bytes == 0) {
        let_go(at);
      } else if (at.live() == own_bytes) {
        run.push_back(at);
      } else {
        kept.push_back(at);
      }
    }
    // The writer, a page of the run and where in it, never passes the record
    // it moves next: a record that doesn't fit after the writer in its page
    // lies in a later page.
    std::size_t to_page = 0;
    std::size_t used = 0;
    for (const page from : run) {
      const auto [first, last] = records_in(from);
      for (auto r = first; r != last; ++r) {
        const record<C> moving(r->address);
        const std::size_t size = moving.size();
        if (used + size > page::room_for_records) {
          run[to_page].set_used(used);
          ++to_page;
          used = 0;
        }
        const page to = run[to_page];
        // The record, and its new place, which only records moved already
        // held, lie in pages where only the leaf's records live; the map
        // holds the leaf exclusively, so no thread reads them.
        std::byte* at = to.records_start() + used;
        std::memmove(at, moving.address(), size);
        if (r->is_target) {
          r->key->template stored_record<C>().retarget(record<C>(at));
        } else {
          (void)r->key->replace(record<C>(at));
        }
        from.move_live(to, size);
        used += size;
      }
    }
    leaf_heap<C>& annex = leaf.annex();
    annex.set_head(page());
    for (auto at = kept.rbegin(); at != kept.rend(); ++at) {
      at->set_next(annex.head());
      annex.set_head(*at);
    }
    if (to_page < run.size()) {
      run[to_page].set_used(used);
      for (std::size_t empty = to_page + 1; empty < run.size(); ++empty) {
        // Every record of the page moved out, and no chain holds it any more.
        run[empty].deallocate(allocator_);
      }
      for (std::size_t at = 0; at <= to_page; ++at) {
        run[at].set_next(annex.head());
        annex.set_head(run[at]);
      }
    }
    for (std::size_t index = 0; index < leaf.key_count(); ++index) {
      const store_key& key = leaf.key(index);
      if (&key != gone && !leaf.is_head(key) && !in_chain(key.stored_record<C>().address()) &&
          !pull(leaf, key)) {
        return;
      }
    }
  }

  // Lets `in` go from its chain: frees it if no record lives in it, else
  // orphans it.
  void let_go(page in) noexcept {
    if (in.live() > 0) {
      orphans_.fetch_add(1, std::memory_order_acq_rel);
      if (!in.orphan()) {
        return;
      }
      // Another leaf killed its last record meanwhile.
      orphans_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // No record lives in the page, and no chain holds it.
    in.deallocate(allocator_);
  }

  // Moves the records of `leaf`'s keys that lie in another leaf's chain and
  // are two generations or more behind the leaf into its head, once a
  // generation; but the head's, which stays put (top of the file). If a page
  // is not to be had, stops, to try again.
  void pull_stale(store_leaf& leaf) noexcept {
    const std::uint8_t generation = leaf.annex().generation();
    if (leaf.annex().pulled() == generation) {
      return;
    }
    for (std::size_t index = leaf.has_head() ? 1 : 0; index < leaf.key_count(); ++index) {
      // The chain first, from the record's address: only a record elsewhere
      // is read.
      const store_key& key = leaf.key(index);
      const record<C> stored = key.stored_record<C>();
      if (leaf.annex().holds(page::of_record(stored.address())) ||
          generation_count::since(generation, stored.generation()) < 2) {
        continue;
      }
      if (!pull(leaf, key)) {
        return;
      }
    }
    leaf.annex().set_pulled(generation);
  }

  // Moves the record of `key`, of `leaf` but not its head, which lies in
  // another leaf's page, into a page of `leaf`'s chain with room, or a new
  // one; false if no page is to be had. A stub moves alone, its target
  // where it is.
  bool pull(store_leaf& leaf, const store_key& key) noexcept {
    const record<C> stored = key.stored_record<C>();
    const std::size_t size = stored.size();
    leaf_heap<C>& annex = leaf.annex();
    std::byte* at = annex.room(size);
    if (at == nullptr) {
      at = annex.room_in_new_page(size, allocator_);
    }
    if (at == nullptr) {
      return false;
    }
    // The room is the record's size, in a page of the leaf's; the map holds
    // the key's leaf exclusively.
    (void)key.replace(stored.copy_to(at, annex.generation()));
    // Its bytes die where they were; its overflow pages are the copy's.
    (void)kill_in(page::of_record(stored.address()), size);
    return true;
  }

  A allocator_;
  // How many orphans hold live records (`heap_page.hpp`).
  std::atomic<std::size_t> orphans_{0};
  // Whether a value took overflow pages since the store was made.
  std::atomic<bool> overflowed_{false};
};

}  // namespace bbtree_store::detail
