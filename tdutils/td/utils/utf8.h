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

#include "td/utils/Slice.h"
#include "td/utils/common.h"

namespace td {

/// checks UTF-8 string for correctness
bool check_utf8(CSlice str);

/// checks if a code unit is a first code unit of a UTF-8 character
inline bool is_utf8_character_first_code_unit(unsigned char c) {
  return (c & 0xC0) != 0x80;
}

/// returns length of UTF-8 string in characters
inline size_t utf8_length(Slice str) {
  size_t result = 0;
  for (auto c : str) {
    result += is_utf8_character_first_code_unit(c);
  }
  return result;
}

/// returns length of UTF-8 string in UTF-16 code units
size_t utf8_utf16_length(Slice str);

/// appends a Unicode character using UTF-8 encoding
template <class T>
void append_utf8_character(T &str, uint32 code) {
  if (code <= 0x7f) {
    str.push_back(static_cast<char>(code));
  } else if (code <= 0x7ff) {
    str.push_back(static_cast<char>(0xc0 | (code >> 6)));  // implementation-defined
    str.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code <= 0xffff) {
    str.push_back(static_cast<char>(0xe0 | (code >> 12)));  // implementation-defined
    str.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    str.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    str.push_back(static_cast<char>(0xf0 | (code >> 18)));  // implementation-defined
    str.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    str.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    str.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

/// moves pointer one UTF-8 character back
inline const unsigned char *prev_utf8_unsafe(const unsigned char *ptr) {
  while (!is_utf8_character_first_code_unit(*--ptr)) {
    // pass
  }
  return ptr;
}

/// moves pointer one UTF-8 character forward and saves code of the skipped character in *code
const unsigned char *next_utf8_unsafe(const unsigned char *ptr, uint32 *code);

/// appends a Unicode character using UTF-8 encoding and returns updated pointer
unsigned char *append_utf8_character_unsafe(unsigned char *ptr, uint32 code);

/// truncates UTF-8 string to the given length in Unicode characters
template <class T>
T utf8_truncate(T str, size_t length) {
  if (str.size() > length) {
    for (size_t i = 0; i < str.size(); i++) {
      if (is_utf8_character_first_code_unit(static_cast<unsigned char>(str[i]))) {
        if (length == 0) {
          return str.substr(0, i);
        } else {
          length--;
        }
      }
    }
  }
  return str;
}

/// truncates UTF-8 string to the given length given in UTF-16 code units
Slice utf8_utf16_truncate(Slice str, size_t length);

template <class T>
T utf8_substr(T str, size_t offset) {
  if (offset == 0) {
    return str;
  }
  auto offset_pos = utf8_truncate(str, offset).size();
  return str.substr(offset_pos);
}

template <class T>
T utf8_substr(T str, size_t offset, size_t length) {
  return utf8_truncate(utf8_substr(str, offset), length);
}

Slice utf8_utf16_substr(Slice str, size_t offset);

Slice utf8_utf16_substr(Slice str, size_t offset, size_t length);

/// Returns UTF-8 string converted to lower case.
string utf8_to_lower(Slice str);

/// Returns UTF-8 string split by words for search.
vector<string> utf8_get_search_words(Slice str);

/// Returns UTF-8 string prepared for search, leaving only digits and lowercased letters.
string utf8_prepare_search_string(Slice str);

/// Returns valid UTF-8 representation of the string.
string utf8_encode(CSlice data);

}  // namespace td
