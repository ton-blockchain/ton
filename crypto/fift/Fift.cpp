/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "Fift.h"
#include "IntCtx.h"
#include "words.h"

#include "td/utils/PathView.h"

namespace fift {

Fift::Fift(Config config) : config_(std::move(config)) {
}

Fift::Config& Fift::config() {
  return config_;
}

td::Result<int> Fift::interpret_file(std::string fname, std::string current_dir, bool is_interactive) {
  auto r_file = config_.source_lookup.lookup_source(fname, current_dir);
  if (r_file.is_error()) {
    return td::Status::Error("cannot locate file `" + fname + "`");
  }
  auto file = r_file.move_as_ok();
  std::stringstream ss(file.data);
  IntCtx ctx{ss, td::PathView(file.path).file_name().str(), td::PathView(file.path).parent_dir().str(),
             (int)!is_interactive};
  return do_interpret(ctx, is_interactive);
}

td::Result<int> Fift::interpret_istream(std::istream& stream, std::string current_dir, bool is_interactive) {
  IntCtx ctx{stream, "stdin", current_dir, (int)!is_interactive};
  return do_interpret(ctx, is_interactive);
}

td::Result<int> Fift::do_interpret(IntCtx& ctx, bool is_interactive) {
  ctx.ton_db = &config_.ton_db;
  ctx.source_lookup = &config_.source_lookup;
  ctx.dictionary = ctx.main_dictionary = ctx.context = config_.dictionary;
  ctx.output_stream = config_.output_stream;
  ctx.error_stream = config_.error_stream;
  if (!ctx.output_stream) {
    return td::Status::Error("Cannot run interpreter without output_stream");
  }
  while (true) {
    auto res = ctx.run(td::make_ref<InterpretCont>());
    if (res.is_error()) {
      res = ctx.add_error_loc(res.move_as_error());
      if (config_.show_backtrace) {
        std::ostringstream os;
        ctx.print_error_backtrace(os);
        LOG(ERROR) << os.str();
      }
      if (is_interactive) {
        LOG(ERROR) << res.move_as_error().message();
        ctx.top_ctx();
        ctx.clear_error();
        ctx.stack.clear();
        ctx.parser->load_next_line();
        continue;
      }
    }
    return res;
  }
}

}  // namespace fift
