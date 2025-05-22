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
*/
#include "src-file.h"
#include "compiler-state.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace tolk {

static_assert(sizeof(SrcLocation) == 8);

const SrcFile* AllRegisteredSrcFiles::find_file(const std::string& abs_filename) const {
  for (const SrcFile* file : all_src_files) {
    if (file->abs_filename == abs_filename) {
      return file;
    }
  }
  return nullptr;
}

const SrcFile* AllRegisteredSrcFiles::locate_and_register_source_file(const std::string& rel_filename, SrcLocation included_from) {
  td::Result<std::string> path = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::Realpath, rel_filename.c_str());
  if (path.is_error()) {
    if (included_from.is_defined()) {
      throw ParseError(included_from, "Failed to import: " + path.move_as_error().message().str());
    }
    throw Fatal("Failed to locate " + rel_filename + ": " + path.move_as_error().message().str());
  }

  std::string abs_filename = path.move_as_ok();
  if (const SrcFile* file = find_file(abs_filename)) {
    return file;
  }

  td::Result<std::string> text = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, abs_filename.c_str());
  if (text.is_error()) {
    if (included_from.is_defined()) {
      throw ParseError(included_from, "Failed to import: " + text.move_as_error().message().str());
    }
    throw Fatal("Failed to read " + rel_filename + ": " + text.move_as_error().message().str());
  }

  int file_id = static_cast<int>(all_src_files.size());   // SrcFile::file_id is the index in all files
  SrcFile* created = new SrcFile(file_id, rel_filename, std::move(abs_filename), text.move_as_ok());
  if (G.is_verbosity(1)) {
    std::cerr << "register file_id " << created->file_id << " " << created->abs_filename << std::endl;
  }
  all_src_files.push_back(created);
  return created;
}

SrcFile* AllRegisteredSrcFiles::get_next_unparsed_file() {
  int last_registered_file_id = static_cast<int>(all_src_files.size() - 1);
  if (last_parsed_file_id >= last_registered_file_id) {
    return nullptr;
  }
  return const_cast<SrcFile*>(all_src_files[++last_parsed_file_id]);
}

bool SrcFile::is_stdlib_file() const {
  std::string_view rel(rel_filename);
  return rel.size() > 10 && rel.substr(0, 8) == "@stdlib/"; // common.tolk, tvm-dicts.tolk, etc
}

bool SrcFile::is_offset_valid(int offset) const {
  return offset >= 0 && offset < static_cast<int>(text.size());
}

SrcFile::SrcPosition SrcFile::convert_offset(int offset) const {
  if (!is_offset_valid(offset)) {
    return SrcPosition{offset, -1, -1, "invalid offset"};
  }

  // currently, converting offset to line number is O(N): just read file contents char by char and detect lines
  // since original Tolk src lines are now printed into Fift output, this is invoked for every asm instruction
  // but anyway, it consumes a small amount of time relative to other work of the compiler
  // in the future, it can be optimized by making lines index aside just std::string_view text
  int line_idx = 0;
  int char_idx = 0;
  int line_offset = 0;
  for (int i = 0; i < offset; ++i) {
    char c = text[i];
    if (c == '\n') {
      line_idx++;
      char_idx = 0;
      line_offset = i + 1;
    } else {
      char_idx++;
    }
  }

  size_t line_len = text.size() - line_offset;
  for (int i = line_offset; i < static_cast<int>(text.size()); ++i) {
    if (text[i] == '\n') {
      line_len = i - line_offset;
      break;
    }
  }

  std::string_view line_str(text.data() + line_offset, line_len);
  return SrcPosition{offset, line_idx + 1, char_idx + 1, line_str};
}


std::ostream& operator<<(std::ostream& os, const SrcFile* src_file) {
  return os << (src_file ? src_file->rel_filename : "unknown-location");
}

std::ostream& operator<<(std::ostream& os, const Fatal& fatal) {
  return os << fatal.what();
}

const SrcFile* SrcLocation::get_src_file() const {
  return G.all_src_files.get_file(file_id);
}

void SrcLocation::show(std::ostream& os) const {
  const SrcFile* src_file = get_src_file();
  os << src_file;
  if (src_file && src_file->is_offset_valid(char_offset)) {
    SrcFile::SrcPosition pos = src_file->convert_offset(char_offset);
    os << ':' << pos.line_no;
    if (pos.char_no != 1) {
      os << ':' << pos.char_no;
    }
  }
}

void SrcLocation::show_context(std::ostream& os) const {
  const SrcFile* src_file = get_src_file();
  if (!src_file || !src_file->is_offset_valid(char_offset)) {
    return;
  }
  SrcFile::SrcPosition pos = src_file->convert_offset(char_offset);
  os << std::right << std::setw(4) << pos.line_no << " | ";
  os << pos.line_str << "\n";

  os << "    " << " | ";
  for (int i = 1; i < pos.char_no; ++i) {
    os << ' ';
  }
  os << '^' << "\n";
}

// when generating Fift output, every block of asm instructions originated from the same Tolk line,
// is preceded by an original line as a comment
void SrcLocation::show_line_to_fif_output(std::ostream& os, int indent, int* last_line_no) const {
  SrcFile::SrcPosition pos = G.all_src_files.get_file(file_id)->convert_offset(char_offset);
  
  // avoid duplicating one line multiple times in fift output
  if (pos.line_no == *last_line_no) {
    return;
  }
  *last_line_no = pos.line_no;

  // trim some characters from start and end to see `else if (x)` not `} else if (x) {`
  std::string_view s = pos.line_str;
  int b = 0, e = static_cast<int>(s.size() - 1);
  while (std::isspace(s[b]) || s[b] == '}') {
    if (b < e) b++;
    else break;
  }
  while (std::isspace(s[e]) || s[e] == '{' || s[e] == ';' || s[e] == ',') {
    if (e > b) e--;
    else break;
  }

  if (b < e) {
    for (int i = 0; i < indent * 2; ++i) {
      os << ' ';
    }
    os << "// " << pos.line_no << ": " << s.substr(b, e - b + 1) << std::endl;
  }
}

std::string SrcLocation::to_string() const {
  std::ostringstream os;
  show(os);
  return os.str();
}

std::ostream& operator<<(std::ostream& os, SrcLocation loc) {
  loc.show(os);
  return os;
}

void SrcLocation::show_general_error(std::ostream& os, const std::string& message, const std::string& err_type) const {
  show(os);
  if (!err_type.empty()) {
    os << ": " << err_type;
  }
  os << ": " << message << std::endl;
  show_context(os);
}

void SrcLocation::show_note(const std::string& err_msg) const {
  show_general_error(std::cerr, err_msg, "note");
}

void SrcLocation::show_warning(const std::string& err_msg) const {
  show_general_error(std::cerr, err_msg, "warning");
}

void SrcLocation::show_error(const std::string& err_msg) const {
  show_general_error(std::cerr, err_msg, "error");
}

std::ostream& operator<<(std::ostream& os, const ParseError& error) {
  error.show(os);
  return os;
}

void ParseError::show(std::ostream& os) const {
  if (message.find('\n') == std::string::npos) {
    // just print a single-line message
    os << loc << ": error: " << message << std::endl;
  } else {
    // print "location: line1 \n (spaces) line2 \n ..."
    std::string_view message = this->message;
    std::string loc_text = loc.to_string();
    std::string loc_spaces(std::min(static_cast<int>(loc_text.size()), 30), ' ');
    size_t start = 0, end;
    os << loc_text << ": error: ";
    while ((end = message.find('\n', start)) != std::string::npos) {
      if (start > 0) {
        os << loc_spaces << "  ";
      }
      os << message.substr(start, end - start) << std::endl;
      start = end + 1;
    }
    if (start < message.size()) {
      os << loc_spaces << "  " << message.substr(start) << std::endl;
    }
  }
  if (current_function) {
    os << "    // in function `" << current_function->as_human_readable() << "`" << std::endl;
  }
  loc.show_context(os);
}

}  // namespace tolk
