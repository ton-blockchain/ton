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
#include "compiler-settings.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace tolk {

static_assert(sizeof(SrcRange) == 12);

SrcFilePtr AllRegisteredSrcFiles::find_file(const std::string& realpath) const {
  // files with the same realpath are considered equal
  for (SrcFilePtr file : all_src_files) {
    if (file->realpath == realpath) {
      return file;
    }
  }
  return nullptr;
}

SrcFilePtr AllRegisteredSrcFiles::locate_and_register_source_file(const std::string& filename, AnyV v_import_filename) {
  bool is_stdlib = filename.size() > 8 && filename.starts_with("@stdlib/");

  td::Result<std::string> path = G_settings.read_callback(CompilerSettings::FsReadCallbackKind::Realpath, filename.c_str(), G_settings.callback_payload);
  if (path.is_error()) {
    if (v_import_filename) {
      err("Failed to import: {}", path.move_as_error().message().str()).fire(v_import_filename);
    }
    throw Fatal("Failed to locate " + filename + ": " + path.move_as_error().message().str());
  }

  std::string realpath = path.move_as_ok();
  if (SrcFilePtr file = find_file(realpath)) {
    return file;
  }

  td::Result<std::string> text = G_settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, realpath.c_str(), G_settings.callback_payload);
  if (text.is_error()) {
    if (v_import_filename) {
      err("Failed to import: {}", text.move_as_error().message().str()).fire(v_import_filename);
    }
    throw Fatal("Failed to read " + realpath + ": " + text.move_as_error().message().str());
  }

  int file_id = static_cast<int>(all_src_files.size());   // SrcFile::file_id is the index in all files
  SrcFile* created = new SrcFile(file_id, is_stdlib, std::move(realpath), text.move_as_ok());
  if (G_settings.verbosity >= 1) {
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

SrcFile::SrcFile(int file_id, bool is_stdlib_file, std::string realpath, std::string&& file_text)
  : file_id(file_id)
  , is_stdlib_file(is_stdlib_file)
  , realpath(std::move(realpath))
  , text(std::move(file_text))
  , text_len(static_cast<int>(text.size()))
  , contract_directive(nullptr) {
  // build a sorted index of line starts once; `convert_offset` uses upper_bound on it
  line_offsets.reserve(text_len / 40 + 2);   // rough estimate: ~40 chars per line
  line_offsets.push_back(0);
  for (int i = 0; i < text_len; ++i) {
    if (text[i] == '\n') {
      line_offsets.push_back(i + 1);
    }
  }
}

void SrcFile::assign_contract_directive(ContractDirective* contract_directive) {
  this->contract_directive = contract_directive;
}

SrcFile::SrcPosition SrcFile::convert_offset(int offset) const {
  if (!is_offset_valid(offset)) {
    return SrcPosition{-1, -1, offset, "invalid offset"};
  }

  // line_offsets[i] = start offset of line i; find the last start <= offset
  auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
  --it;
  int line_idx = static_cast<int>(it - line_offsets.begin());
  int line_offset = *it;
  int char_idx = offset - line_offset;

  int line_len;
  if (line_idx + 1 < static_cast<int>(line_offsets.size())) {
    line_len = line_offsets[line_idx + 1] - line_offset - 1;   // exclude trailing '\n'
  } else {
    line_len = text_len - line_offset;
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

SrcFilePtr SrcRange::get_src_file() const {
  return G.all_src_files.get_file(file_id);
}

std::string SrcRange::stringify_start_location(bool output_char_no) const {
  SrcFilePtr src_file = get_src_file();
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


SrcRange::DecodedRange SrcRange::decode_offsets() const {
  SrcFilePtr src_file = get_src_file();
  tolk_assert(src_file && src_file->is_offset_valid(start_offset));

  SrcFile::SrcPosition pos_s = src_file->convert_offset(start_offset);
  SrcFile::SrcPosition pos_e = src_file->convert_offset(end_offset);
  return {
    .file_id = file_id,
    .start_line_no = pos_s.line_no,
    .start_char_no = pos_s.char_no,
    .end_line_no = pos_e.line_no,
    .end_char_no = pos_e.char_no,
    .start_line_str = pos_s.line_str,
    .end_line_str = pos_e.line_str,
    .text_inside = std::string_view(src_file->text).substr(start_offset, end_offset - start_offset)
  };
}

static bool contains_only_spaces(std::string_view s) {
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// True when the underlined range is essentially the whole statement on its line,
// e.g. `return x;` or `    break; // comment`
// Then we'll also output a line above, for better context.
static bool occupies_almost_whole_line(const SrcFile::SrcPosition& start, const SrcFile::SrcPosition& end) {
  if (start.line_no != end.line_no) {
    return false;
  }

  // before: only spaces
  std::string_view before = start.line_str.substr(0, start.char_no - 1);
  if (!contains_only_spaces(before)) {
    return false;
  }

  // after: spaces, semicolon, optional comment
  std::string_view after = start.line_str.substr(end.char_no - 1);
  size_t i = 0;
  while (i < after.size() && (std::isspace(static_cast<unsigned char>(after[i])) || after[i] == ';')) {
    ++i;
  }
  if (i >= after.size()) {
    return true;
  }
  return i + 1 < after.size() && after[i] == '/' && after[i + 1] == '/';
}

void SrcRange::output_underlined(std::ostream& os, const char* ansi_gutter, const char* ansi_underline, const char* ansi_reset) const {
  SrcFilePtr src_file = get_src_file();
  if (!src_file || !src_file->is_offset_valid(end_offset) || !is_valid()) {
    return;
  }
  SrcFile::SrcPosition start = src_file->convert_offset(start_offset);
  SrcFile::SrcPosition end = src_file->convert_offset(end_offset);

  os << ansi_gutter << "    " << " | " << ansi_reset << std::endl;

  if (occupies_almost_whole_line(start, end) && start.line_no > 1) {
    SrcFile::SrcPosition prev = src_file->convert_offset(src_file->line_offsets[start.line_no - 2]);
    if (!contains_only_spaces(prev.line_str)) {
      os << ansi_gutter << "    " << " | " << ansi_reset << prev.line_str << "\n";
    }
  }

  os << ansi_gutter << std::right << std::setw(4) << start.line_no << " | " << ansi_reset << start.line_str << "\n";
  os << ansi_gutter << "    " << " | " << ansi_reset;
  for (int i = 1; i < start.char_no; ++i) {
    os << ' ';
  }
  int end_char_no_first_line = start.line_no == end.line_no ? end.char_no - 1 : static_cast<int>(start.line_str.size());
  os << ansi_underline;
  for (int i = start.char_no; i <= end_char_no_first_line; ++i) {
    os << '^';
  }
  os << ansi_reset << "\n";

  if (end.line_no > start.line_no + 1) {
    os << ansi_gutter << " ..." << "   ..." << ansi_reset << "\n";
  }
  if (end.line_no > start.line_no) {
    os << ansi_gutter << std::right << std::setw(4) << end.line_no << " | " << ansi_reset << end.line_str << "\n";
    os << ansi_gutter << "    " << " | " << ansi_reset << ansi_underline;
    bool was_non_space = false;
    for (int i = 1; i < end.char_no; ++i) {
      was_non_space |= !std::isspace(end.line_str[i - 1]);
      os << (was_non_space ? '^' : ' ');
    }
    os << ansi_reset << "\n";
  }
}


}  // namespace tolk
