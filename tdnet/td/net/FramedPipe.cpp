#include "td/net/FramedPipe.h"
#include "td/utils/as.h"

namespace td {

Status framed_write(ChainBufferWriter &writer, Slice message) {
  if (message.size() > MAX_FRAMED_MESSAGE_SIZE) {
    return Status::Error("Too large message");
  }
  char head[4];
  as<uint32>(head) = static_cast<uint32>(message.size());
  writer.append(Slice(head, 4));
  writer.append(message);
  return Status::OK();
}

Result<size_t> framed_read(ChainBufferReader &reader, BufferSlice &dst) {
  auto input = reader.clone();
  if (input.size() < 4) {
    return 4;
  }
  uint32 size = as<uint32>(input.cut_head(4).move_as_buffer_slice().as_slice().data());

  // Validate size to prevent integer overflow and excessive memory allocation
  if (size > MAX_FRAMED_MESSAGE_SIZE) {
    return Status::Error(PSLICE() << "Invalid message size: " << size << " (max: " << MAX_FRAMED_MESSAGE_SIZE << ")");
  }

  // Check if we have enough data
  if (input.size() < size) {
    return size + 4;
  }

  // Extract the message
  dst = input.cut_head(size).move_as_buffer_slice();
  reader = std::move(input);
  return 0;
}

}  // namespace td
