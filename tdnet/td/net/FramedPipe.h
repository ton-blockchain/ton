#pragma once

#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"

namespace td {

// Simple framing protocol: 4-byte length prefix + message
// Maximum message size to prevent excessive memory allocation
constexpr size_t MAX_FRAMED_MESSAGE_SIZE = 16536;

// Write a framed message to a ChainBufferWriter
// Format: [4-byte length][message data]
Status framed_write(ChainBufferWriter &writer, Slice message);

// Read a framed message from a ChainBufferReader
// Returns:
//   - 0 if a complete message was read (stored in dst)
//   - >0 indicating how many more bytes are needed
//   - Error if message size is invalid
Result<size_t> framed_read(ChainBufferReader &reader, BufferSlice &dst);

}  // namespace td
