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
#pragma once

#include "crypto/vm/stack.hpp"
#include "crypto/vm/box.hpp"
#include "crypto/common/bitstring.h"

#include "td/utils/Status.h"

#include "Dictionary.h"
#include "Continuation.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace vm {
class TonDbImpl;  // from crypto/vm/db/TonDb.h
using TonDb = std::unique_ptr<TonDbImpl>;
}  // namespace vm

namespace fift {
class Dictionary;
class SourceLookup;

struct IntError {
  std::string msg;
  IntError(std::string _msg) : msg(_msg) {
  }
};

class CharClassifier {
  unsigned char data_[64];

 public:
  CharClassifier() {
    std::memset(data_, 0, sizeof(data_));
  }
  CharClassifier(td::Slice str, int space_cls = 3) : CharClassifier() {
    import_from_string(str, space_cls);
  }
  CharClassifier(std::string str, int space_cls = 3) : CharClassifier(td::Slice{str}, space_cls) {
  }
  CharClassifier(const char* str, int space_cls = 3) : CharClassifier(td::Slice{str}, space_cls) {
  }
  void import_from_string(td::Slice str, int space_cls = 3);
  void import_from_string(std::string str, int space_cls = 3);
  void import_from_string(const char* str, int space_cls = 3);
  static CharClassifier from_string(td::Slice str, int space_cls = 3);
  void set_char_class(int c, int cl);
  int classify(int c) const {
    c &= 0xff;
    int offs = (c & 3) * 2;
    return (data_[(unsigned)c >> 2] >> offs) & 3;
  }
};

struct ParseCtx {
  int include_depth{0};
  int line_no{0};
  bool need_line{true};
  std::string filename;
  std::string currentd_dir;
  std::istream* input_stream{nullptr};
  std::unique_ptr<std::istream> input_stream_holder;
  std::string word;

 private:
  std::string str;
  const char* input_ptr = nullptr;

 public:
  ParseCtx() = default;
  ParseCtx(std::istream& _istream, std::string _filename, std::string _curdir = "", int _depth = 0)
      : include_depth(_depth)
      , filename(std::move(_filename))
      , currentd_dir(std::move(_curdir))
      , input_stream(&_istream) {
  }
  ParseCtx(std::unique_ptr<std::istream> _istream_ptr, std::string _filename, std::string _curdir = "", int _depth = 0)
      : include_depth(_depth)
      , filename(std::move(_filename))
      , currentd_dir(std::move(_curdir))
      , input_stream(_istream_ptr.get())
      , input_stream_holder(std::move(_istream_ptr)) {
  }

  td::Slice scan_word_to(char delim, bool err_endl = true);
  td::Slice scan_word();
  td::Slice scan_word_ext(const CharClassifier& classifier);
  void skipspc(bool skip_eol = false);

  bool eof() const {
    return !*input_stream;
  }

  bool not_eof() const {
    return !eof();
  }

  void set_input(std::string input_str) {
    str = input_str;
    input_ptr = str.c_str();
    ++line_no;
  }
  void set_input(const char* ptr) {
    input_ptr = ptr;
  }
  const char* get_input() const {
    return input_ptr;
  }

  bool load_next_line();
  bool load_next_line_ifreq() {
    return need_line && load_next_line();
  }

  bool is_sb() const;

  std::ostream& show_context(std::ostream& os) const;
};

struct IntCtx {
  vm::Stack stack;
  Ref<FiftCont> next, exc_handler;
  Ref<FiftCont> exc_cont, exc_next;
  int state{0};
  int exit_code{0};
  td::Status error;

  std::unique_ptr<ParseCtx> parser;
  std::vector<std::unique_ptr<ParseCtx>> parser_save_stack;

  std::ostream* output_stream{nullptr};  // move to OutCtx?
  std::ostream* error_stream{nullptr};

  vm::TonDb* ton_db{nullptr};
  SourceLookup* source_lookup{nullptr};
  int* now{nullptr};

  Dictionary dictionary, main_dictionary, context;

 public:
  IntCtx() = default;
  IntCtx(std::istream& _istream, std::string _filename, std::string _curdir = "", int _depth = 0) {
    parser = std::make_unique<ParseCtx>(_istream, _filename, _curdir, _depth);
  }
  IntCtx(std::unique_ptr<std::istream> _istream, std::string _filename, std::string _curdir = "", int _depth = 0) {
    parser = std::make_unique<ParseCtx>(std::move(_istream), _filename, _curdir, _depth);
  }

  bool enter_ctx(std::unique_ptr<ParseCtx> new_ctx);
  bool enter_ctx(std::string new_filename, std::string new_current_dir, std::unique_ptr<std::istream> new_input_stream);
  bool leave_ctx();
  bool top_ctx();
  int include_depth() const {
    return parser ? parser->include_depth : -1;
  }

  operator vm::Stack &() {
    return stack;
  }

  void clear() {
    state = 0;
    stack.clear();
  }

  void check_compile() const;
  void check_execute() const;
  void check_not_int_exec() const;
  void check_int_exec() const;

  bool print_error_backtrace(std::ostream& os) const;
  bool print_backtrace(std::ostream& os, Ref<FiftCont> cont) const;

  td::Status add_error_loc(td::Status err) const;

  void set_exit_code(int err_code) {
    exit_code = err_code;
  }
  int get_exit_code() const {
    return exit_code;
  }

  void clear_error();
  td::Result<int> get_result();

  Ref<FiftCont> throw_exception(td::Status err, Ref<FiftCont> cur = {});
  td::Result<int> run(Ref<FiftCont> cont);
};

td::StringBuilder& operator<<(td::StringBuilder& os, const ParseCtx& ctx);
std::ostream& operator<<(std::ostream& os, const ParseCtx& ctx);
}  // namespace fift
