#include "Miner.h"

#include "td/utils/Random.h"
#include "td/utils/misc.h"
#include "td/utils/crypto.h"
#include "td/utils/port/Clocks.h"
#include <openssl/sha.h>

namespace ton {
#pragma pack(push, 1)
struct HData {
  unsigned char op[4];
  signed char flags = -4;
  unsigned char expire[4] = {}, myaddr[32] = {}, rdata1[32] = {}, pseed[16] = {}, rdata2[32] = {};
  void inc() {
    for (long i = 31; !(rdata1[i] = ++(rdata2[i])); --i) {
    }
  }
  void set_expire(unsigned x) {
    for (int i = 3; i >= 0; --i) {
      expire[i] = (x & 0xff);
      x >>= 8;
    }
  }

  td::Slice as_slice() const {
    return td::Slice(reinterpret_cast<const td::uint8*>(this), sizeof(*this));
  }
};

struct HDataEnv {
  unsigned char d1 = 0, d2 = sizeof(HData) * 2;
  HData body;

  td::Slice as_slice() const {
    return td::Slice(reinterpret_cast<const td::uint8*>(this), sizeof(*this));
  }

  void init(const block::StdAddress& my_address, td::Slice seed) {
    std::memcpy(body.myaddr, my_address.addr.data(), sizeof(body.myaddr));
    body.flags = static_cast<td::int8>(my_address.workchain * 4 + my_address.bounceable);
    CHECK(seed.size() == 16);
    std::memcpy(body.pseed, seed.data(), 16);
    std::memcpy(body.op, "Mine", 4);

    td::Random::secure_bytes(body.rdata1, 32);
    std::memcpy(body.rdata2, body.rdata1, 32);
  }
};

static_assert(std::is_trivially_copyable<HDataEnv>::value, "HDataEnv must be a trivial type");
#pragma pack(pop)

td::optional<std::string> Miner::run(const Options& options) {
  HDataEnv H;
  H.init(options.my_address, td::Slice(options.seed.data(), options.seed.size()));

  td::Slice data = H.as_slice();
  CHECK(data.size() == 123);

  constexpr size_t prefix_size = 72;
  constexpr size_t guard_pos = prefix_size - (72 - 28);
  CHECK(0 <= guard_pos && guard_pos < 32);
  size_t got_prefix_size = (const unsigned char*)H.body.rdata1 + guard_pos + 1 - (const unsigned char*)&H;
  CHECK(prefix_size == got_prefix_size);

  auto head = data.substr(0, prefix_size);
  auto tail = data.substr(prefix_size);

  SHA256_CTX shactx1, shactx2;
  std::array<td::uint8, 32> hash;
  SHA256_Init(&shactx1);
  auto guard = head.back();

  td::int64 i = 0, i0 = 0;
  for (; i < options.max_iterations; i++) {
    if (!(i & 0xfffff) || head.back() != guard) {
      if (options.hashes_computed) {
        *options.hashes_computed += i - i0;
      }
      i0 = i;
      if (options.expire_at && options.expire_at.value().is_in_past(td::Timestamp::now())) {
        break;
      }
      H.body.set_expire((unsigned)td::Clocks::system() + 900);
      guard = head.back();
      SHA256_Init(&shactx1);
      SHA256_Update(&shactx1, head.ubegin(), head.size());
    }
    shactx2 = shactx1;
    SHA256_Update(&shactx2, tail.ubegin(), tail.size());
    SHA256_Final(hash.data(), &shactx2);

    if (memcmp(hash.data(), options.complexity.data(), 32) < 0) {
      // FOUND
      if (options.hashes_computed) {
        *options.hashes_computed += i - i0;
      }
      return H.body.as_slice().str();
    }
    H.body.inc();
  }
  if (options.hashes_computed) {
    *options.hashes_computed += i - i0;
  }
  return {};
}
}  // namespace ton
