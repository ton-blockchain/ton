#pragma once

#include <string>

namespace td {

class StringCase {
 public:
  static bool is_camel_case(const std::string& input_str);
  static bool is_snake_case(const std::string& input_str);
  static std::string camel_to_snake(const std::string& input_str);
  static std::string snake_to_camel(const std::string& input_str);
};

}
