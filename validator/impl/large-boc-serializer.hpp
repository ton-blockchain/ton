#pragma once
#include "ton/ton-types.h"
#include "vm/cells.h"
#include "td/utils/port/path.h"
#include "vm/db/DynamicBagOfCellsDb.h"

namespace ton {
namespace validator {

td::Status serialize_large_boc_to_file(std::shared_ptr<vm::CellDbReader> reader, vm::Cell::Hash root_hash,
                                       td::FileFd& fd, int mode = 0);

}
}