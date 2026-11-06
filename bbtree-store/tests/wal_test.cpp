// The WAL on storage in memory: what a writer appends, an open replays,
// across files, lost outcomes and a second writer; corruption is an error.
// And the encoding the Rust crate's Verus proofs cover, checked instead:
// records round-trip, whatever follows them; any flipped bit fails; a scan
// reads exactly what appends wrote (the Rust crate's `tests/wal` and
// `wal/record.rs` tests).
#include "memory_storage.hpp"

#include <bbtree_store/wal/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace wal = bbtree_store::wal;
using bbtree_store_test::append_fault;
using bbtree_store_test::memory_storage;
using byte_string = std::vector<std::uint8_t>;

byte_string bytes(std::string_view text) { return {text.begin(), text.end()}; }

// A record an open replayed, owning its bytes.
struct replayed {
  bool is_insert;
  byte_string key;
  byte_string value;

  static replayed insert(std::string_view key, std::string_view value) {
    return {true, bytes(key), bytes(value)};
  }
  static replayed remove(std::string_view key) { return {false, bytes(key), {}}; }
  friend bool operator==(const replayed&, const replayed&) = default;
};

// An opened WAL: its writer, and what the open replayed.
template <class S>
class opened {
 public:
  explicit opened(S storage)
      : writer_(
            wal::wal_writer<S>::open(storage, [this](wal::record_address address, const wal::record& record) {
              replayed_.push_back({record.kind == wal::record::kind_type::insert,
                                   byte_string(record.key.begin(), record.key.end()),
                                   byte_string(record.value.begin(), record.value.end())});
              addresses_.push_back(address);
            })) {}

  wal::wal_writer<S>& writer() noexcept { return writer_; }
  const std::vector<replayed>& replays() const noexcept { return replayed_; }
  const std::vector<wal::record_address>& addresses() const noexcept { return addresses_; }

  // Appends an insert of `key`, alone.
  wal::record_address insert(std::string_view key, std::string_view value) {
    const byte_string k = bytes(key);
    const byte_string v = bytes(value);
    return writer_.append(std::array{wal::record::insert(k, v)})[0];
  }

  // Appends a removal of `key`, alone.
  wal::record_address remove(std::string_view key) {
    const byte_string k = bytes(key);
    return writer_.append(std::array{wal::record::remove(k)})[0];
  }

 private:
  // Filled by the open, which runs first: declared before the writer.
  std::vector<replayed> replayed_;
  std::vector<wal::record_address> addresses_;
  wal::wal_writer<S> writer_;
};

// The kind of the `wal_error` that `operation` throws; none if it throws
// none.
template <class Operation>
std::optional<wal::wal_error> error_of(Operation&& operation) {
  try {
    operation();
  } catch (const wal::wal_error& error) {
    return error;
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("CRC32C matches the standard check value", "[wal]") {
  const byte_string check = bytes("123456789");
  CHECK(wal::detail::crc32c::of(check) == 0xE3069283);
  CHECK(wal::detail::crc32c::of({}) == 0);
  // RFC 3720, B.4: 32 bytes of zeros.
  CHECK(wal::detail::crc32c::of(byte_string(32, 0)) == 0x8A9136AA);
}

TEST_CASE("any flipped bit fails a record's checks", "[wal]") {
  const byte_string key = bytes("key");
  const wal::detail::stored_record record{UINT64_MAX, wal::detail::stored_kind::remove, false, key, {}};
  byte_string encoded;
  record.encode(encoded);
  REQUIRE(encoded.size() == record.len());
  REQUIRE(wal::detail::stored_record::decode(encoded).has_value());
  for (std::size_t bit = 0; bit < encoded.size() * 8; ++bit) {
    byte_string flipped = encoded;
    flipped[bit / 8] = static_cast<std::uint8_t>(flipped[bit / 8] ^ (1u << (bit % 8)));
    INFO("bit " << bit);
    CHECK_FALSE(wal::detail::stored_record::decode(flipped).has_value());
  }
}

TEST_CASE("records round-trip, whatever follows them", "[wal]") {
  std::mt19937_64 rng(1);
  for (int round = 0; round < 500; ++round) {
    byte_string key(rng() % 40);
    byte_string value(rng() % 300);
    for (auto& byte : key) {
      byte = static_cast<std::uint8_t>(rng());
    }
    for (auto& byte : value) {
      byte = static_cast<std::uint8_t>(rng());
    }
    const auto kind = static_cast<wal::detail::stored_kind>(1 + rng() % 3);
    const wal::detail::stored_record record{rng(), kind, rng() % 2 == 0, key, value};
    byte_string encoded;
    record.encode(encoded);
    // Whatever follows: nothing, garbage, another record.
    encoded.resize(encoded.size() + rng() % 50, static_cast<std::uint8_t>(rng()));
    const std::optional<wal::detail::stored_record> decoded = wal::detail::stored_record::decode(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->lsn == record.lsn);
    CHECK(decoded->kind == record.kind);
    CHECK(decoded->last_in_append == record.last_in_append);
    CHECK(byte_string(decoded->key.begin(), decoded->key.end()) == key);
    CHECK(byte_string(decoded->value.begin(), decoded->value.end()) == value);
    // A record cut short decodes to nothing.
    const std::span<const std::uint8_t> cut(encoded.data(), record.len() - 1 - rng() % (record.len() - 1));
    CHECK_FALSE(wal::detail::stored_record::decode(cut).has_value());
  }
}

TEST_CASE("a scan reads exactly what appends wrote", "[wal]") {
  const byte_string a = bytes("a");
  const byte_string one = bytes("1");
  const wal::detail::append_part first[] = {{wal::detail::stored_kind::insert, a, one},
                                            {wal::detail::stored_kind::remove, a, {}}};
  const wal::detail::append_part second[] = {{wal::detail::stored_kind::claim, one, {}}};
  const wal::detail::wal_tail start;
  const wal::detail::encoded_append append1(start, first);
  const wal::detail::encoded_append append2(append1.tail(), second);
  byte_string file(append1.encoded().begin(), append1.encoded().end());
  file.insert(file.end(), append2.encoded().begin(), append2.encoded().end());
  const wal::detail::scanned_bytes scanned = start.scan_bytes(file);
  REQUIRE(scanned.holds_records);
  CHECK(scanned.records.size() == 3);
  std::vector<wal::record_address> addresses = append1.addresses();
  addresses.insert(addresses.end(), append2.addresses().begin(), append2.addresses().end());
  CHECK(scanned.addresses == addresses);
  CHECK(scanned.tail.file_len == append2.tail().file_len);
  CHECK(scanned.tail.file_appends == 2);
  CHECK(scanned.tail.next_lsn == 3);
  // A scan picks up where it stopped.
  const wal::detail::scanned_bytes rest =
      append1.tail().scan_bytes(std::span<const std::uint8_t>(file).subspan(append1.encoded().size()));
  REQUIRE(rest.holds_records);
  CHECK(rest.addresses == append2.addresses());
  // An append cut short holds no records past the last whole append.
  const wal::detail::scanned_bytes cut =
      start.scan_bytes(std::span<const std::uint8_t>(file).first(file.size() - 1));
  CHECK_FALSE(cut.holds_records);
  CHECK(cut.stopped_at == append1.encoded().size());
  // An append's first record alone ends no append.
  const wal::detail::scanned_bytes partial =
      start.scan_bytes(std::span<const std::uint8_t>(file).first(append1.addresses()[1].offset()));
  CHECK_FALSE(partial.holds_records);
  CHECK(partial.stopped_at == wal::detail::file_header.size());
}

TEST_CASE("an open replays what was appended", "[wal]") {
  const memory_storage<> storage;
  opened wal_opened(storage);
  CHECK(wal_opened.replays().empty());
  const byte_string a = bytes("a");
  const byte_string b = bytes("b");
  const byte_string one = bytes("1");
  std::vector<wal::record_address> addresses =
      wal_opened.writer().append(std::array{wal::record::insert(a, one), wal::record::insert(b, {})});
  addresses.push_back(wal_opened.remove("a"));
  CHECK(wal_opened.writer().append(std::span<const wal::record>()).empty());

  const opened reopened(storage);
  const std::vector<replayed> expected = {replayed::insert("a", "1"), replayed::insert("b", ""),
                                          replayed::remove("a")};
  CHECK(reopened.replays() == expected);
  CHECK(reopened.addresses() == addresses);

  CHECK(addresses[0].read_value(storage) == one);
  CHECK(addresses[1].read_value(storage).empty());
  // A removal has no value.
  const std::optional<wal::wal_error> read = error_of([&] { (void)addresses[2].read_value(storage); });
  REQUIRE(read);
  CHECK(read->kind() == wal::wal_error_kind::corrupted);
}

TEST_CASE("a full file starts the next", "[wal]") {
  // Two appends a file: the open's claim and one insert fill file 0.
  const memory_storage<2> storage;
  std::vector<replayed> expected;
  {
    opened wal_opened(storage);
    for (std::uint8_t i = 0; i < 5; ++i) {
      const std::string_view key(reinterpret_cast<const char*>(&i), 1);
      const std::uint8_t pair[] = {i, i};
      const std::string_view value(reinterpret_cast<const char*>(pair), 2);
      wal_opened.insert(key, value);
      expected.push_back(replayed::insert(key, value));
    }
  }
  CHECK(storage.len(2).has_value());
  CHECK_FALSE(storage.len(3).has_value());

  // File 2 is full: this open's claim starts file 3.
  opened wal_opened(storage);
  CHECK(wal_opened.replays() == expected);
  CHECK(storage.len(3).has_value());
  const std::uint8_t zero = 0;
  const std::string_view key(reinterpret_cast<const char*>(&zero), 1);
  const wal::record_address address = wal_opened.remove(key);
  expected.push_back(replayed::remove(key));

  const opened reopened(storage);
  CHECK(reopened.replays() == expected);
  REQUIRE_FALSE(reopened.addresses().empty());
  CHECK(reopened.addresses().back() == address);
}

TEST_CASE("a lost outcome is written once", "[wal]") {
  for (const append_fault fault : {append_fault::request_lost, append_fault::response_lost}) {
    const memory_storage<> storage;
    opened wal_opened(storage);
    storage.fail_append(fault);
    wal_opened.insert("a", "1");
    wal_opened.insert("b", "2");

    const std::vector<replayed> expected = {replayed::insert("a", "1"), replayed::insert("b", "2")};
    CHECK(opened(storage).replays() == expected);
  }
}

TEST_CASE("unknown outcomes fail the append", "[wal]") {
  const memory_storage<> storage;
  opened wal_opened(storage);
  for (int i = 0; i < 3; ++i) {
    storage.fail_append(append_fault::request_lost);
  }
  const std::optional<wal::wal_error> failed = error_of([&] { wal_opened.insert("a", "1"); });
  REQUIRE(failed);
  CHECK(failed->kind() == wal::wal_error_kind::storage);
  // It wasn't written: the next append goes where it would have.
  wal_opened.insert("b", "2");

  // A response lost three times: the first attempt was written, and the next
  // append's offset mismatches.
  for (int i = 0; i < 3; ++i) {
    storage.fail_append(append_fault::response_lost);
  }
  const std::optional<wal::wal_error> lost = error_of([&] { wal_opened.insert("c", "3"); });
  REQUIRE(lost);
  CHECK(lost->kind() == wal::wal_error_kind::storage);
  const std::optional<wal::wal_error> moved = error_of([&] { wal_opened.insert("d", "4"); });
  REQUIRE(moved);
  CHECK(moved->kind() == wal::wal_error_kind::tail_moved);

  const std::vector<replayed> expected = {replayed::insert("b", "2"), replayed::insert("c", "3")};
  CHECK(opened(storage).replays() == expected);
}

TEST_CASE("a new writer fences the one before", "[wal]") {
  const memory_storage<> storage;
  opened old_writer(storage);
  old_writer.insert("a", "1");

  opened new_writer(storage);
  CHECK(new_writer.replays() == std::vector{replayed::insert("a", "1")});
  const std::optional<wal::wal_error> moved = error_of([&] { old_writer.insert("b", "2"); });
  REQUIRE(moved);
  CHECK(moved->kind() == wal::wal_error_kind::tail_moved);
  new_writer.insert("c", "3");

  const std::vector<replayed> expected = {replayed::insert("a", "1"), replayed::insert("c", "3")};
  CHECK(opened(storage).replays() == expected);
}

TEST_CASE("corruption fails the open", "[wal]") {
  {
    const memory_storage<> storage;
    opened wal_opened(storage);
    wal_opened.insert("a", "1");
    const std::uint64_t end = *storage.len(0);
    const byte_string garbage = bytes("not a record");
    REQUIRE(storage.append(0, end, garbage).status == wal::append_status::appended);
    const std::optional<wal::wal_error> corrupted = error_of([&] { opened reopened(storage); });
    REQUIRE(corrupted);
    CHECK(corrupted->kind() == wal::wal_error_kind::corrupted);
    CHECK(corrupted->file() == 0);
    CHECK(corrupted->offset() == end);
  }
  // A file of another format version.
  const memory_storage<> storage;
  const std::uint8_t header[] = {'b', 'b', 't', 'r', 'e', 'e', 'W', 'L', 2, 0, 0, 0};
  REQUIRE(storage.append(0, 0, header).status == wal::append_status::appended);
  const std::optional<wal::wal_error> corrupted = error_of([&] { opened reopened(storage); });
  REQUIRE(corrupted);
  CHECK(corrupted->kind() == wal::wal_error_kind::corrupted);
  CHECK(corrupted->file() == 0);
  CHECK(corrupted->offset() == 0);
}
