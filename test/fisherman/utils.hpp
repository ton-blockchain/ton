#pragma once

#include "td/utils/buffer.h"
#include "td/utils/Status.h"
#include "td/utils/JsonBuilder.h"
#include "ton/ton-types.h"

namespace test::fisherman {

td::Result<td::BufferSlice> read_file_to_buffer(const std::string &path);

/// \brief Parses a JsonValue object to build a BlockIdExt.
/// \param jv A td::JsonValue that should be a JSON object with fields:
///     "workchain_id" (int),
///     "shard_id" (hex string),
///     "seqno" (int >= 0).
/// \return A td::Result containing either the constructed BlockIdExt or an error status.
td::Result<ton::BlockIdExt> parse_block_id_from_json(td::JsonValue jv);

}  // namespace test::fisherman
