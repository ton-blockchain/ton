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

namespace tolk {

struct SrcFile {
  struct SrcPosition {
    int offset;
    int line_no;
    int char_no;
    std::string_view line_str;
  };

  struct ImportDirective {
    const SrcFile* imported_file;
  };

  int file_id;                          // an incremental counter through all parsed files
  std::string rel_filename;             // relative to cwd
  std::string abs_filename;             // absolute from root
  std::string text;                     // file contents loaded into memory, every Token::str_val points inside it
  AnyV ast = nullptr;                   // when a file has been parsed, its ast_tolk_file is kept here
  std::vector<ImportDirective> imports; // to check strictness (can't use a symbol without importing its file)

  SrcFile(int file_id, std::string rel_filename, std::string abs_filename, std::string&& text)
    : file_id(file_id)
    , rel_filename(std::move(rel_filename))
    , abs_filename(std::move(abs_filename))
    , text(std::move(text)) { }

  SrcFile(const SrcFile& other) = delete;
  SrcFile &operator=(const SrcFile&) = delete;

  bool is_stdlib_file() const;

  bool is_offset_valid(int offset) const;
  SrcPosition convert_offset(int offset) const;
};


// SrcLocation points to a location (line, column) in some loaded .tolk source SrcFile.
// Note, that instead of storing src_file, line_no, etc., only 2 ints are stored.
// The purpose is: sizeof(SrcLocation) == 8, so it's just passed/stored without pointers/refs, just like int64_t.
// When decoding SrcLocation into human-readable format, it's converted to SrcFile::SrcPosition via offset.
class SrcLocation {
  friend class Lexer;

  int file_id = -1;       // = SrcFile::file_id (note, that get_src_file() does linear search)
  int char_offset = -1;   // offset from SrcFile::text

public:

  SrcLocation() = default;
  explicit SrcLocation(const SrcFile* src_file) : file_id(src_file->file_id) {
  }

  bool is_defined() const { return file_id != -1; }
  bool is_stdlib() const { return file_id == 0; }
  const SrcFile* get_src_file() const;

  // similar to `this->get_src_file() == symbol->get_src_file() || symbol->get_src_file()->is_stdlib()`
  // (but effectively, avoiding linear search)
  bool is_symbol_from_same_or_builtin_file(SrcLocation symbol_loc) const {
    return file_id == symbol_loc.file_id || symbol_loc.file_id < 1;
  }

  void show(std::ostream& os) const;
  void show_context(std::ostream& os) const;
  void show_line_to_fif_output(std::ostream& os, int indent, int* last_line_no) const;
  std::string to_string() const;

  void show_general_error(std::ostream& os, const std::string& message, const std::string& err_type) const;
  void show_note(const std::string& err_msg) const;
  void show_warning(const std::string& err_msg) const;
  void show_error(const std::string& err_msg) const;
};

std::ostream& operator<<(std::ostream& os, SrcLocation loc);

class AllRegisteredSrcFiles {
  std::vector<const SrcFile*> all_src_files;
  int last_parsed_file_id = -1;

public:
  const SrcFile* get_file(int file_id) const { return all_src_files.at(file_id); }
  const SrcFile* find_file(const std::string& abs_filename) const;

  const SrcFile* locate_and_register_source_file(const std::string& rel_filename, SrcLocation included_from);
  SrcFile* get_next_unparsed_file();

  auto begin() const { return all_src_files.begin(); }
  auto end() const { return all_src_files.end(); }
};

struct Fatal final : std::exception {
  std::string message;

  explicit Fatal(std::string _msg) : message(std::move(_msg)) {
  }
  const char* what() const noexcept override {
    return message.c_str();
  }
};

std::ostream& operator<<(std::ostream& os, const Fatal& fatal);

struct ParseError : std::exception {
  FunctionPtr current_function;
  SrcLocation loc;
  std::string message;

  ParseError(SrcLocation loc, std::string message)
    : current_function(nullptr), loc(loc), message(std::move(message)) {}
  ParseError(FunctionPtr current_function, SrcLocation loc, std::string message)
    : current_function(current_function), loc(loc), message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }
  void show(std::ostream& os) const;
};

std::ostream& operator<<(std::ostream& os, const ParseError& error);

}  // namespace tolk
