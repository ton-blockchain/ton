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
#include "IntCtx.h"

namespace fift {

td::StringBuilder& operator<<(td::StringBuilder& os, const ParseCtx& ctx) {
  if (ctx.include_depth) {
    return os << ctx.filename << ":" << ctx.line_no << ": ";
  } else {
    return os;
  }
}

std::ostream& operator<<(std::ostream& os, const ParseCtx& ctx) {
  return os << (PSLICE() << ctx).c_str();
}

void CharClassifier::import_from_string(td::Slice str, int space_cls) {
  set_char_class(' ', space_cls);
  set_char_class('\t', space_cls);
  int cls = 3;
  for (char c : str) {
    if (c == ' ') {
      cls--;
    } else {
      set_char_class(c, cls);
    }
  }
}

void CharClassifier::import_from_string(std::string str, int space_cls) {
  import_from_string(td::Slice{str}, space_cls);
}

void CharClassifier::import_from_string(const char* str, int space_cls) {
  import_from_string(td::Slice{str}, space_cls);
}

CharClassifier CharClassifier::from_string(td::Slice str, int space_cls) {
  return CharClassifier{str, space_cls};
}

void CharClassifier::set_char_class(int c, int cl) {
  c &= 0xff;
  cl &= 3;
  int offs = (c & 3) * 2;
  int mask = (3 << offs);
  cl <<= offs;
  unsigned char* p = data_ + (c >> 2);
  *p = static_cast<unsigned char>((*p & ~mask) | cl);
}

bool ParseCtx::load_next_line() {
  if (!std::getline(*input_stream, str)) {
    return false;
  }
  need_line = false;
  if (!str.empty() && str.back() == '\r') {
    str.pop_back();
  }
  set_input(str);
  return true;
}

bool ParseCtx::is_sb() const {
  return !eof() && line_no == 1 && *input_ptr == '#' && input_ptr[1] == '!';
}

td::Slice ParseCtx::scan_word_to(char delim, bool err_endl) {
  load_next_line_ifreq();
  auto ptr = input_ptr;
  while (*ptr && *ptr != delim) {
    ptr++;
  }
  if (*ptr) {
    std::swap(ptr, input_ptr);
    return td::Slice{ptr, input_ptr++};
  } else if (err_endl && delim) {
    throw IntError{std::string{"end delimiter `"} + delim + "` not found"};
  } else {
    need_line = true;
    std::swap(ptr, input_ptr);
    return td::Slice{ptr, input_ptr};
  }
}

td::Slice ParseCtx::scan_word() {
  skipspc(true);
  auto ptr = input_ptr;
  while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '\r') {
    ptr++;
  }
  auto ptr2 = ptr;
  std::swap(ptr, input_ptr);
  skipspc();
  return td::Slice{ptr, ptr2};
}

td::Slice ParseCtx::scan_word_ext(const CharClassifier& classifier) {
  skipspc(true);
  auto ptr = input_ptr;
  while (*ptr && *ptr != '\r' && *ptr != '\n') {
    int c = classifier.classify(*ptr);
    if ((c & 1) && ptr != input_ptr) {
      break;
    }
    ptr++;
    if (c & 2) {
      break;
    }
  }
  std::swap(ptr, input_ptr);
  return td::Slice{ptr, input_ptr};
}

void ParseCtx::skipspc(bool skip_eol) {
  do {
    while (*input_ptr == ' ' || *input_ptr == '\t' || *input_ptr == '\r') {
      ++input_ptr;
    }
    if (!skip_eol || *input_ptr) {
      break;
    }
  } while (load_next_line());
}

bool IntCtx::enter_ctx(std::unique_ptr<ParseCtx> new_parser) {
  if (!new_parser) {
    return false;
  }
  if (parser) {
    parser_save_stack.push_back(std::move(parser));
  }
  parser = std::move(new_parser);
  return true;
}

bool IntCtx::enter_ctx(std::string new_filename, std::string new_current_dir,
                       std::unique_ptr<std::istream> new_input_stream) {
  if (!new_input_stream) {
    return false;
  } else {
    return enter_ctx(
        std::make_unique<ParseCtx>(std::move(new_input_stream), new_filename, new_current_dir, include_depth() + 1));
  }
}

bool IntCtx::leave_ctx() {
  if (parser_save_stack.empty()) {
    return false;
  } else {
    parser = std::move(parser_save_stack.back());
    parser_save_stack.pop_back();
    return true;
  }
}

bool IntCtx::top_ctx() {
  if (!parser_save_stack.empty()) {
    parser = std::move(parser_save_stack[0]);
    parser_save_stack.clear();
  }
  return true;
}

void IntCtx::check_compile() const {
  if (state <= 0) {
    throw IntError{"compilation mode only"};
  }
}

void IntCtx::check_execute() const {
  if (state != 0) {
    throw IntError{"interpret mode only"};
  }
}

void IntCtx::check_not_int_exec() const {
  if (state < 0) {
    throw IntError{"not allowed in internal interpret mode"};
  }
}

void IntCtx::check_int_exec() const {
  if (state >= 0) {
    throw IntError{"internal interpret mode only"};
  }
}

bool IntCtx::print_error_backtrace(std::ostream& os) const {
  if (exc_cont.is_null() && exc_next.is_null()) {
    os << "(no backtrace)\n";
    return false;
  }
  if (exc_cont.not_null()) {
    os << "top: ";
    exc_cont->dump(os, *this);
  }
  return print_backtrace(os, exc_next);
}

bool IntCtx::print_backtrace(std::ostream& os, Ref<FiftCont> cont) const {
  for (int i = 1; cont.not_null() && i <= 16; i++) {
    os << "level " << i << ": ";
    cont->dump(os, *this);
    cont = cont->up();
  }
  if (cont.not_null()) {
    os << "... more levels ...\n";
  }
  return true;
}

Ref<FiftCont> IntCtx::throw_exception(td::Status err, Ref<FiftCont> cur) {
  exc_cont = std::move(cur);
  exc_next = std::move(next);
  error = std::move(err);
  next.clear();
  auto cont = std::move(exc_handler);
  if (cont.is_null()) {
    return {};  // no Fift exception handler set
  } else if (cont.is_unique()) {
    return cont.unique_write().handle_modify(*this);
  } else {
    return cont->handle_tail(*this);
  }
}

void IntCtx::clear_error() {
  error = td::Status::OK();
  exit_code = 0;
}

td::Result<int> IntCtx::get_result() {
  if (error.is_error()) {
    return error.move_as_error();
  } else {
    return exit_code;
  }
}

std::ostream& ParseCtx::show_context(std::ostream& os) const {
  if (include_depth && line_no) {
    os << filename << ":" << line_no << ":\t";
  }
  if (!word.empty()) {
    os << word << ":";
  }
  return os;
}

td::Status IntCtx::add_error_loc(td::Status err) const {
  if (err.is_error() && parser) {
    std::ostringstream os;
    parser->show_context(os);
    return err.move_as_error_prefix(os.str());
  } else {
    return err;
  }
}

td::Result<int> IntCtx::run(Ref<FiftCont> cont) {
  clear_error();
  while (cont.not_null()) {
    try {
      if (cont.is_unique()) {
        cont = cont.unique_write().run_modify(*this);
      } else {
        cont = cont->run_tail(*this);
      }
    } catch (IntError& err) {
      cont = throw_exception(td::Status::Error(err.msg), std::move(cont));
    } catch (vm::VmError& err) {
      cont = throw_exception(err.as_status(), std::move(cont));
    } catch (vm::VmVirtError& err) {
      cont = throw_exception(err.as_status(), std::move(cont));
    } catch (vm::CellBuilder::CellWriteError&) {
      cont = throw_exception(td::Status::Error("Cell builder write error"), std::move(cont));
    } catch (vm::VmFatal&) {
      cont = throw_exception(td::Status::Error("fatal vm error"), std::move(cont));
    }
    if (cont.is_null()) {
      cont = std::move(next);
    }
  }
  return get_result();
}

}  // namespace fift
