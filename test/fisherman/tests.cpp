#include "block_reader.hpp"

#include "crypto/block/block-auto.h"
#include "utils.hpp"

using namespace test::fisherman;

auto main(int argc, char **argv) -> int {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " /path/to/rootdb config.json\n";
    return 1;
  }

  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));  // TODO: add to config

  std::string db_path = argv[1];
  std::string json_file_path = argv[2];

  auto content_res = read_file_to_buffer(json_file_path);
  if (content_res.is_error()) {
    std::cerr << "Error reading JSON file: " << content_res.error().message().str() << std::endl;
    return 1;
  }
  td::BufferSlice content = content_res.move_as_ok();

  td::Parser parser(content.as_slice());
  auto decode_result = do_json_decode(parser, 100);
  if (decode_result.is_error()) {
    std::cerr << "JSON parse error: " << decode_result.error().message().str() << std::endl;
    return 1;
  }

  auto block_id_res = parse_block_id_from_json(decode_result.move_as_ok());
  if (block_id_res.is_error()) {
    std::cerr << "Error extracting BlockIdExt: " << block_id_res.error().message().str() << std::endl;
    return 1;
  }
  ton::BlockIdExt block_id = block_id_res.move_as_ok();

  BlockDataLoader loader(db_path);

  auto block_data_result = loader.load_block_data(block_id);
  if (block_data_result.is_error()) {
    std::cerr << "Error loading block data: " << block_data_result.error().message().str() << std::endl;
    return 1;
  }

  auto block_data = block_data_result.move_as_ok();
  LOG(INFO) << "BlockId: " << block_data->block_id().to_str();
  LOG(INFO) << "Block data size: " << block_data->data().size() << " bytes";

  LOG(INFO) << "Cell has block record = " << block::gen::Block().validate_ref(10000000, block_data->root_cell()) << "\n";

  std::ostringstream os;
  block::gen::Block().print_ref(os, block_data->root_cell());
  LOG(INFO) << "Block = " << os.str();

  block::gen::Block::Record block_rec;
  bool ok = block::gen::Block().cell_unpack(block_data->root_cell(), block_rec);
  CHECK(ok);

  block::gen::BlockInfo::Record info_rec;
  block::gen::BlockInfo().cell_unpack(block_rec.info, info_rec);
  LOG(INFO) << "start_lt = " << info_rec.start_lt << ", end_lt = " << info_rec.end_lt;

  block::gen::ShardIdent::Record shard_rec;
  block::gen::ShardIdent().unpack(info_rec.shard.write(), shard_rec);
  LOG(INFO) << "workchain_id = " << shard_rec.workchain_id;

  return 0;
}
