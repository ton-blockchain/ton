#include "block_reader.hpp"

#include "crypto/block/block-auto.h"
#include "block_manipulator/factory.hpp"
#include "utils.hpp"

using namespace test::fisherman;

void print_block(const block::gen::Block::Record &block_rec) {
  std::ostringstream os;
  td::Ref<vm::Cell> block_cell_pack;
  CHECK(block::gen::t_Block.cell_pack(block_cell_pack, block_rec));
  block::gen::t_Block.print_ref(os, block_cell_pack);
  LOG(INFO) << "Block = " << os.str();
}

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

  auto js = decode_result.move_as_ok();
  auto &js_obj = js.get_object();
  auto blk_id_obj_res = td::get_json_object_field(js_obj, "block_id", td::JsonValue::Type::Object, false);
  CHECK(blk_id_obj_res.is_ok());
  auto blk_id_res = parse_block_id_from_json(blk_id_obj_res.move_as_ok());
  if (blk_id_res.is_error()) {
    std::cerr << "Error extracting BlockIdExt: " << blk_id_res.error().message().str() << std::endl;
    return 1;
  }
  ton::BlockIdExt blk_id = blk_id_res.move_as_ok();

  BlockDataLoader loader(db_path);

  auto blk_data_result = loader.load_block_data(blk_id);
  if (blk_data_result.is_error()) {
    std::cerr << "Error loading block data: " << blk_data_result.error().message().str() << std::endl;
    return 1;
  }

  auto blk_data = blk_data_result.move_as_ok();

  block::gen::Block::Record block_rec;
  CHECK(block::gen::t_Block.cell_unpack(blk_data->root_cell(), block_rec));

  print_block(block_rec);

  auto manipulation_config = td::get_json_object_field(js_obj, "manipulation", td::JsonValue::Type::Object, false);
  CHECK(manipulation_config.is_ok());
  ManipulatorFactory().create(manipulation_config.move_as_ok())->modify(block_rec);

  LOG(INFO) << "Block after manipulation:";
  print_block(block_rec);
  return 0;
}
