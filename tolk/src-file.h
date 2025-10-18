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
#pragma once

#include <string>
#include <vector>
#include "fwd-declarations.h"
#include "symtable.h"

namespace tolk {

struct SrcFile {
  struct SrcPosition {
    int line_no;
    int char_no;
    int line_offset;
    std::string_view line_str;
  };

  struct ImportDirective {
    const SrcFile* imported_file;
  };

  int file_id;                          // an incremental counter through all parsed files
  bool is_stdlib_file;                  // is a part of Tolk distribution, imported via "@stdlib/..."
  std::string realpath;                 // what "realpath" returned to locate (either abs path or what tolk-js returns)
  std::string text;                     // file contents loaded into memory, every Token::str_val points inside it
  AnyV ast = nullptr;                   // when a file has been parsed, its ast_tolk_file is kept here
  std::vector<ImportDirective> imports; // to check strictness (can't use a symbol without importing its file)

  SrcFile(int file_id, bool is_stdlib_file, std::string realpath, std::string&& text)
    : file_id(file_id)
    , is_stdlib_file(is_stdlib_file)
    , realpath(std::move(realpath))
    , text(std::move(text)) { }

  SrcFile(const SrcFile& other) = delete;
  SrcFile &operator=(const SrcFile&) = delete;

  bool is_offset_valid(int offset) const;
  SrcPosition convert_offset(int offset) const;

  std::string extract_short_name() const;
  std::string extract_dirname() const;
};

class AllRegisteredSrcFiles {
  std::vector<const SrcFile*> all_src_files;
  int last_parsed_file_id = -1;

public:
  const SrcFile* get_file(int file_id) const { return all_src_files.at(file_id); }
  const SrcFile* find_file(const std::string& realpath) const;

  const SrcFile* locate_and_register_source_file(const std::string& filename, AnyV v_import_filename);
  SrcFile* get_next_unparsed_file();

  auto begin() const { return all_src_files.begin(); }
  auto end() const { return all_src_files.end(); }
};

// SrcRange is a "substring" in some loaded .tolk source SrcFile.
// Note, that instead of storing src_file, line_no, etc., only 3 ints are stored.
// When decoding SrcRange into human-readable format, it's converted to SrcFile::SrcPosition via offset.
class SrcRange {
  int file_id;        // SrcFile::file_id (note, that get_src_file() does linear search)
  int start_offset;   // offset from SrcFile::text
  int end_offset;     // >= start_offset, otherwise "invalid range"

  SrcRange(int file_id, int start_offset, int end_offset)
    : file_id(file_id)
    , start_offset(start_offset)
    , end_offset(end_offset) {
  }

public:
  static SrcRange undefined() {
    return SrcRange(-1, 0, -1);
  }

  static SrcRange overlap(SrcRange start, SrcRange end) {
    return SrcRange(start.file_id, start.start_offset, end.end_offset);
  }
  
  static SrcRange empty_at_start(SrcRange range) {
    return SrcRange(range.file_id, range.start_offset, range.start_offset);
  }
  
  static SrcRange empty_at_end(SrcRange range) {
    return SrcRange(range.file_id, range.end_offset, range.end_offset);
  }

  static SrcRange span(SrcRange start, int len) {
    return SrcRange(start.file_id, start.start_offset, start.start_offset + len);
  }

  static SrcRange span(int file_id, int start_offset, int len) {
    return SrcRange(file_id, start_offset, start_offset + len);
  }

  static SrcRange unclosed_range(int file_id, int start_offset) {
    return SrcRange(file_id, start_offset, 0);    // in dev mode, there is `assert` that range is closed before used
  }

  void end(SrcRange end) {
    end_offset = end.end_offset;
  }

  bool is_defined() const {
    return file_id != -1;
  }
  bool is_valid() const {
    return end_offset >= start_offset;
  }
  bool is_file_id_same_or_stdlib_common(SrcRange other) const {
    return file_id == 0 || file_id == other.file_id;
  }

  const SrcFile* get_src_file() const;
  std::string stringify_start_location(bool output_char_no) const;

  void output_first_line_to_fif(std::ostream& os, int indent) const;
  void output_underlined(std::ostream& os) const;
};

}  // namespace tolk
