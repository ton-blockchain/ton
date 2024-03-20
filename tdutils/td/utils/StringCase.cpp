#include "StringCase.h"
#include <cctype>

namespace td {

bool StringCase::is_camel_case(const std::string& input_str) {
  if (input_str.empty() || !::islower(input_str[0]))
    return false;

  for (char c : input_str) {
    if (::isupper(c)) {
      return true;
    }
  }

  return false;
}

bool StringCase::is_snake_case(const std::string& input_str) {
  if (input_str.empty() || !::islower(input_str[0]))
    return false;

  for (char c : input_str) {
    if (::isupper(c) || c == ' ') {
      return false;
    }
  }

  return true;
}


std::string StringCase::camel_to_snake(const std::string& input_str) {
  std::string result;
  result.reserve(input_str.size() + std::count_if(input_str.begin(), input_str.end(), ::isupper));

  if (!input_str.empty()) {
    result += static_cast<char>(::tolower(input_str[0]));
  }

  for (size_t i = 1; i < input_str.size(); ++i) {
    if (::isupper(input_str[i])) {
      result += "_";
    }
    result += static_cast<char>(::tolower(input_str[i]));
  }

  return result;
}


std::string StringCase::snake_to_camel(const std::string& input_str) {
  std::string result;
  result.reserve(input_str.size());
  bool capitalizeNext = false;

  for (char c : input_str) {
    if (c == '_') {
      capitalizeNext = true;
    } else {
      result += capitalizeNext ? static_cast<char>(::toupper(c)) : c;
      capitalizeNext = false;
    }
  }

  return result;
}

}
