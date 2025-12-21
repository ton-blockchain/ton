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
#include "compilation-errors.h"
#include "compiler-state.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace tolk {

static_assert(sizeof(SrcRange) == 12);

const SrcFile* AllRegisteredSrcFiles::find_file(const std::string& realpath) const {
  // files with the same realpath are considered equal
  for (const SrcFile* file : all_src_files) {
    if (file->realpath == realpath) {
      return file;
    }
  }
  return nullptr;
}

const SrcFile* AllRegisteredSrcFiles::locate_and_register_source_file(const std::string& filename, AnyV v_import_filename) {
  bool is_stdlib = filename.size() > 8 && filename.starts_with("@stdlib/");

  td::Result<std::string> path = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::Realpath, filename.c_str());
  if (path.is_error()) {
    if (v_import_filename) {
      err("Failed to import: {}", path.move_as_error().message().str()).fire(v_import_filename);
    }
    throw Fatal("Failed to locate " + filename + ": " + path.move_as_error().message().str());
  }

  std::string realpath = path.move_as_ok();
  if (const SrcFile* file = find_file(realpath)) {
    return file;
  }

  td::Result<std::string> text = G.settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, realpath.c_str());
  if (text.is_error()) {
    if (v_import_filename) {
      err("Failed to import: {}", text.move_as_error().message().str()).fire(v_import_filename);
    }
    throw Fatal("Failed to read " + realpath + ": " + text.move_as_error().message().str());
  }

  int file_id = static_cast<int>(all_src_files.size());   // SrcFile::file_id is the index in all files
  SrcFile* created = new SrcFile(file_id, is_stdlib, std::move(realpath), text.move_as_ok());
  if (G.is_verbosity(1)) {
    std::cerr << "register file_id " << created->file_id << " " << created->realpath << std::endl;
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

bool SrcFile::is_offset_valid(int offset) const {
  return offset >= 0 && offset < static_cast<int>(text.size());
}

SrcFile::SrcPosition SrcFile::convert_offset(int offset) const {
  if (!is_offset_valid(offset)) {
    return SrcPosition{-1, -1, offset, "invalid offset"};
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
  return SrcPosition{line_idx + 1, char_idx + 1,  line_offset, line_str};
}

std::string SrcFile::extract_short_name() const {
  size_t last_slash = realpath.find_last_of("/\\");
  if (last_slash == std::string::npos) {
    return realpath;
  }
  std::string short_name = realpath.substr(last_slash + 1);    // "file.tolk" (no path)

  if (is_stdlib_file) {   // not "common.tolk", but "@stdlib/common"
    return "@stdlib/" + short_name.substr(0, short_name.size() - 5);
  }
  return short_name;
}

std::string SrcFile::extract_dirname() const {
  size_t last_slash = realpath.find_last_of("/\\");
  if (last_slash == std::string::npos) {
    return "";
  }
  return realpath.substr(0, last_slash + 1);
}

const SrcFile* SrcRange::get_src_file() const {
  return G.all_src_files.get_file(file_id);
}


// --------------------------------------------
//    stringify ranges and locations,
//    output to Fift as comment

static void output_line_to_fif(std::ostream& os, int indent, std::string_view line_str, int line_no, bool dots = false) {
  // trim some characters from start and end to see `else if (x)` not `} else if (x) {`
  std::string_view s = line_str;
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
    os << "// " << (dots ? "..." : "") << line_no << ": " << s.substr(b, e - b + 1) << std::endl;
  }  
}

// when generating Fift output, every block of asm instructions originated from the same Tolk line,
// is preceded by an original line as a comment
void SrcRange::output_first_line_to_fif(std::ostream& os, int indent) const {
  // avoid duplicating one line multiple times in fift output
  static int last_start_offset = 0, last_start_line_no = 0;
  static int last_end_offset = 0, last_end_line_no = 0;

  bool just_printed_start_line = false;
  if (start_offset != last_start_offset) {
    SrcFile::SrcPosition pos = G.all_src_files.get_file(file_id)->convert_offset(start_offset);
    if (pos.line_no != last_start_line_no) {
      output_line_to_fif(os, indent, pos.line_str, pos.line_no);
      just_printed_start_line = true;
    }
    last_start_offset = last_end_offset = start_offset;
    last_start_line_no = last_end_line_no = pos.line_no;
  }
  if (end_offset > last_end_offset) {
    SrcFile::SrcPosition pos = G.all_src_files.get_file(file_id)->convert_offset(end_offset);
    if (pos.line_no > last_end_line_no) {
      std::string_view line_str = pos.line_str.substr(0, end_offset - pos.line_offset);
      output_line_to_fif(os, indent, line_str, pos.line_no, just_printed_start_line);
    }
    last_end_offset = end_offset;
    last_end_line_no = pos.line_no;
  }
}

std::string SrcRange::stringify_start_location(bool output_char_no) const {
  const SrcFile* src_file = get_src_file();
  if (!src_file || !src_file->is_offset_valid(start_offset)) {
    return "unknown-location";
  }

  SrcFile::SrcPosition pos = src_file->convert_offset(start_offset);
  std::string s = src_file->realpath;
  s += ':';
  s += std::to_string(pos.line_no);
  if (output_char_no && pos.char_no != 1) {
    s += ':';
    s += std::to_string(pos.char_no);
  }
  return s;
}


void SrcRange::output_underlined(std::ostream& os) const {
  const SrcFile* src_file = get_src_file();
  if (!src_file || !src_file->is_offset_valid(end_offset) || !is_valid()) {
    return;
  }
  SrcFile::SrcPosition start = src_file->convert_offset(start_offset);
  SrcFile::SrcPosition end = src_file->convert_offset(end_offset);

  os << std::right << std::setw(4) << start.line_no << " | " << start.line_str << "\n";
  os << "    " << " | ";
  for (int i = 1; i < start.char_no; ++i) {
    os << ' ';
  }
  int end_char_no_first_line = start.line_no == end.line_no ? end.char_no : static_cast<int>(start.line_str.size());
  for (int i = start.char_no; i < end_char_no_first_line; ++i) {
    os << '^';
  }
  os << "\n";

  if (end.line_no > start.line_no + 1) {
    os << " ..." << "   ...";
    os << "\n";
  }
  if (end.line_no > start.line_no) {
    os << std::right << std::setw(4) << end.line_no << " | " << end.line_str << "\n";
    os << "    " << " | ";
    bool was_non_space = false;
    for (int i = 1; i < end.char_no; ++i) {
      was_non_space |= !std::isspace(end.line_str[i - 1]);
      os << (was_non_space ? '^' : ' ');
    }
    os << "\n";
  }
}


}  // namespace tolk
