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
#include <iostream>

namespace tolk {

extern AllRegisteredSrcFiles all_src_files;
extern std::string stdlib_filename;

static_assert(sizeof(SrcLocation) == 8);

const SrcFile* AllRegisteredSrcFiles::find_file(int file_id) const {
  for (const SrcFile* file : all_src_files) {
    if (file->file_id == file_id) {
      return file;
    }
  }
  return nullptr;
}

const SrcFile* AllRegisteredSrcFiles::find_file(const std::string& abs_filename) const {
  for (const SrcFile* file : all_src_files) {
    if (file->abs_filename == abs_filename) {
      return file;
    }
  }
  return nullptr;
}

const SrcFile* AllRegisteredSrcFiles::register_file(const std::string& rel_filename, const std::string& abs_filename, std::string&& text, const SrcFile* included_from) {
  SrcFile* created = new SrcFile(++last_file_id, rel_filename, abs_filename, std::move(text), included_from);
  all_src_files.push_back(created);
  return created;
}


bool SrcFile::is_entrypoint_file() const {
  return file_id == (stdlib_filename.empty() ? 0 : 1);
}

bool SrcFile::is_offset_valid(int offset) const {
  return offset >= 0 && offset < static_cast<int>(text.size());
}

SrcFile::SrcPosition SrcFile::convert_offset(int offset) const {
  if (!is_offset_valid(offset)) {
    return SrcPosition{offset, -1, -1, "invalid offset"};
  }

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
  return all_src_files.find_file(file_id);
}

void SrcLocation::show(std::ostream& os) const {
  const SrcFile* src_file = get_src_file();
  os << src_file;
  if (src_file && src_file->is_offset_valid(char_offset)) {
    SrcFile::SrcPosition pos = src_file->convert_offset(char_offset);
    os << ':' << pos.line_no <<  ':' << pos.char_no;
  }
}

void SrcLocation::show_context(std::ostream& os) const {
  const SrcFile* src_file = get_src_file();
  if (!src_file || !src_file->is_offset_valid(char_offset)) {
    return;
  }
  SrcFile::SrcPosition pos = src_file->convert_offset(char_offset);
  os << "  "  << pos.line_str << "\n";

  os << "  ";
  for (int i = 1; i < pos.char_no; ++i) {
    os << ' ';
  }
  os << '^' << "\n";
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
  os << where << ": error: " << message << std::endl;
  where.show_context(os);
}

}  // namespace tolk
