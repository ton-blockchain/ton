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
#include "tl_json_converter.h"

#include "td/tl/tl_simple.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/filesystem.h"
#include "td/utils/logging.h"
#include "td/utils/Slice.h"
#include "td/utils/StringBuilder.h"

#include <utility>

namespace td {

using Mode = tl::TL_writer::Mode;

namespace {
std::string tl_name = "ton_api";
}

template <class T>
void gen_to_json_constructor(StringBuilder &sb, const T *constructor, bool is_header) {
  sb << "void to_json(JsonValueScope &jv, "
     << "const " << tl_name << "::" << tl::simple::gen_cpp_name(constructor->name) << " &object)";
  if (is_header) {
    sb << ";\n\n";
    return;
  }
  sb << " {\n";
  sb << "  auto jo = jv.enter_object();\n";
  sb << "  jo(\"@type\", \"" << constructor->name << "\");\n";
  std::vector<std::string> var_names(constructor->var_count);
  for (auto &arg : constructor->args) {
    if (arg.var_num >= 0) {
      CHECK(arg.var_num < (int)var_names.size());
      var_names[arg.var_num] = tl::simple::gen_cpp_field_name(arg.name);
    }
  }
  for (auto &arg : constructor->args) {
    auto field_name = tl::simple::gen_cpp_field_name(arg.name);
    bool is_optional = arg.type->type == tl::simple::Type::Custom || arg.exist_var_num >= 0;

    if (is_optional) {
      sb << "  if (";
      if (arg.type->type == tl::simple::Type::Custom) {
        sb << "object." << field_name;
        if (arg.exist_var_num >= 0) {
          sb << " && ";
        }
      }
      if (arg.exist_var_num >= 0) {
        CHECK(arg.exist_var_num < (int)var_names.size());
        sb << "(object." << var_names[arg.exist_var_num] << " & " << (1 << arg.exist_var_bit) << ")";
      }
      sb << ") {\n  ";
    }
    auto object = PSTRING() << "object." << tl::simple::gen_cpp_field_name(arg.name);
    if (arg.type->type == tl::simple::Type::Bytes || arg.type->type == tl::simple::Type::SecureBytes) {
      object = PSTRING() << "JsonBytes{" << object << "}";
    } else if (arg.type->type == tl::simple::Type::Bool) {
      object = PSTRING() << "JsonBool{" << object << "}";
    } else if (arg.type->type == tl::simple::Type::Int64) {
      object = PSTRING() << "JsonInt64{" << object << "}";
    } else if (arg.type->type == tl::simple::Type::Vector &&
               (arg.type->vector_value_type->type == tl::simple::Type::Bytes ||
                arg.type->vector_value_type->type == tl::simple::Type::SecureBytes)) {
      object = PSTRING() << "JsonVectorBytes(" << object << ")";
    } else if (arg.type->type == tl::simple::Type::Vector &&
               arg.type->vector_value_type->type == tl::simple::Type::Int64) {
      object = PSTRING() << "JsonVectorInt64{" << object << "}";
    }
    sb << "  jo(\"" << arg.name << "\", ToJson(" << object << "));\n";
    if (is_optional) {
      sb << "  }\n";
    }
  }
  sb << "}\n";
}

void gen_to_json(StringBuilder &sb, const tl::simple::Schema &schema, bool is_header) {
  for (auto *custom_type : schema.custom_types) {
    if (custom_type->constructors.size() > 1) {
      auto type_name = tl::simple::gen_cpp_name(custom_type->name);
      sb << "void to_json(JsonValueScope &jv, const " << tl_name << "::" << type_name << " &object)";
      if (is_header) {
        sb << ";\n";
      } else {
        sb << " {\n"
           << "  " << tl_name << "::downcast_call(const_cast<" << tl_name << "::" << type_name
           << " &>(object), [&jv](const auto &object) { "
              "to_json(jv, object); });\n"
           << "}\n";
      }
    }
    for (auto *constructor : custom_type->constructors) {
      gen_to_json_constructor(sb, constructor, is_header);
    }
  }
  for (auto *function : schema.functions) {
    gen_to_json_constructor(sb, function, is_header);
  }

  if (is_header) {
    sb << "inline void to_json(JsonValueScope &jv, const ton::" << tl_name
       << "::Object &object) {\n"
          "  ton::"
       << tl_name << "::downcast_call(const_cast<ton::" << tl_name
       << "::Object &>(object),[&jv](const auto &object) { "
          "to_json(jv, object); });\n"
       << "}\n";
    sb << "inline void to_json(JsonValueScope &jv, const ton::" << tl_name << "::Function &object) {\n"
       << "  ton::" << tl_name << "::downcast_call(const_cast<ton::" << tl_name
       << "::Function &>(object), [&jv](const auto &object) { "
          "to_json(jv, object); });\n"
       << "}\n";
  }
}

template <class T>
void gen_from_json_constructor(StringBuilder &sb, const T *constructor, bool is_header) {
  sb << "Status from_json(" << tl_name << "::" << tl::simple::gen_cpp_name(constructor->name)
     << " &to, JsonObject &from)";
  if (is_header) {
    sb << ";\n";
  } else {
    sb << " {\n";
    for (auto &arg : constructor->args) {
      sb << "  {\n";
      sb << "    TRY_RESULT(value, get_json_object_field(from, \"" << tl::simple::gen_cpp_name(arg.name)
         << "\", JsonValue::Type::Null, true));\n";
      sb << "    if (value.type() != JsonValue::Type::Null) {\n";
      if (arg.type->type == tl::simple::Type::Bytes || arg.type->type == tl::simple::Type::SecureBytes) {
        sb << "      TRY_STATUS(from_json_bytes(to." << tl::simple::gen_cpp_field_name(arg.name)
           << ", std::move(value)));\n";
      } else if (arg.type->type == tl::simple::Type::Vector &&
                 (arg.type->vector_value_type->type == tl::simple::Type::Bytes ||
                  arg.type->vector_value_type->type == tl::simple::Type::SecureBytes)) {
        sb << "      TRY_STATUS(from_json_vector_bytes(to." << tl::simple::gen_cpp_field_name(arg.name)
           << ", std::move(value)));\n";
      } else {
        sb << "      TRY_STATUS(from_json(to." << tl::simple::gen_cpp_field_name(arg.name) << ", std::move(value)));\n";
      }
      sb << "    }\n";
      sb << "  }\n";
    }
    sb << "  return Status::OK();\n";
    sb << "}\n";
  }
}

void gen_from_json(StringBuilder &sb, const tl::simple::Schema &schema, bool is_header, Mode mode) {
  for (auto *custom_type : schema.custom_types) {
    if (!((custom_type->is_query_ && mode != Mode::Client) || (custom_type->is_result_ && mode != Mode::Server)) &&
        mode != Mode::All) {
      continue;
    }
    for (auto *constructor : custom_type->constructors) {
      gen_from_json_constructor(sb, constructor, is_header);
    }
  }
  if (mode == Mode::Client) {
    return;
  }
  for (auto *function : schema.functions) {
    gen_from_json_constructor(sb, function, is_header);
  }
}

using Vec = std::vector<std::pair<int32, std::string>>;
void gen_tl_constructor_from_string(StringBuilder &sb, Slice name, const Vec &vec, bool is_header) {
  sb << "Result<int32> tl_constructor_from_string(" << tl_name << "::" << name << " *object, const std::string &str)";
  if (is_header) {
    sb << ";\n\n";
    return;
  }
  sb << " {\n";
  sb << "  static const std::unordered_map<Slice, int32, SliceHash> m = {\n";

  bool is_first = true;
  for (auto &p : vec) {
    if (is_first) {
      is_first = false;
    } else {
      sb << ",\n";
    }
    sb << "    {\"" << p.second << "\", " << p.first << "}";
  }
  sb << "\n  };\n";
  sb << "  auto it = m.find(str);\n";
  sb << "  if (it == m.end()) {\n"
     << "    return Status::Error(PSLICE() << \"Unknown class \\\"\" << str << \"\\\"\");\n"
     << "  }\n"
     << "  return it->second;\n";
  sb << "}\n\n";
}

void gen_tl_constructor_from_string(StringBuilder &sb, const tl::simple::Schema &schema, bool is_header, Mode mode) {
  Vec vec_for_nullary;
  for (auto *custom_type : schema.custom_types) {
    if (!((custom_type->is_query_ && mode != Mode::Client) || (custom_type->is_result_ && mode != Mode::Server)) &&
        mode != Mode::All) {
      continue;
    }
    Vec vec;
    for (auto *constructor : custom_type->constructors) {
      vec.push_back(std::make_pair(constructor->id, constructor->name));
      vec_for_nullary.push_back(vec.back());
    }

    if (vec.size() > 1) {
      gen_tl_constructor_from_string(sb, tl::simple::gen_cpp_name(custom_type->name), vec, is_header);
    }
  }
  gen_tl_constructor_from_string(sb, "Object", vec_for_nullary, is_header);

  if (mode == Mode::Client) {
    return;
  }
  Vec vec_for_function;
  for (auto *function : schema.functions) {
    vec_for_function.push_back(std::make_pair(function->id, function->name));
  }
  gen_tl_constructor_from_string(sb, "Function", vec_for_function, is_header);
}

void gen_json_converter_file(const tl::simple::Schema &schema, const std::string &file_name_base, bool is_header,
                             Mode mode) {
  auto file_name = is_header ? file_name_base + ".h" : file_name_base + ".cpp";
  //file_name = "auto/" + file_name;
  auto old_file_content = [&] {
    auto r_content = read_file(file_name);
    if (r_content.is_error()) {
      return BufferSlice();
    }
    return r_content.move_as_ok();
  }();

  std::string buf(2000000, ' ');
  StringBuilder sb(MutableSlice{buf});

  if (is_header) {
    sb << "#pragma once\n\n";

    sb << "#include \"auto/tl/" << tl_name << ".h\"\n\n";
    sb << "#include \"auto/tl/" << tl_name << ".hpp\"\n\n";

    sb << "#include \"td/utils/JsonBuilder.h\"\n";
    sb << "#include \"td/utils/Status.h\"\n\n";

    sb << "#include \"crypto/common/bitstring.h\"\n";
  } else {
    sb << "#include \"" << file_name_base << ".h\"\n\n";

    sb << "#include \"auto/tl/" << tl_name << ".h\"\n";
    sb << "#include \"auto/tl/" << tl_name << ".hpp\"\n\n";

    sb << "#include \"tl/tl_json.h\"\n\n";

    sb << "#include \"td/utils/base64.h\"\n";
    sb << "#include \"td/utils/common.h\"\n";
    sb << "#include \"td/utils/Slice.h\"\n\n";

    sb << "#include <functional>\n";
    sb << "#include <unordered_map>\n\n";
  }
  sb << "namespace ton {\n";
  sb << "namespace " << tl_name << "{\n";
  sb << "  using namespace td;\n";
  gen_tl_constructor_from_string(sb, schema, is_header, mode);
  gen_from_json(sb, schema, is_header, mode);
  gen_to_json(sb, schema, is_header);
  sb << "}  // namespace " << tl_name << "\n";
  sb << "}  // namespace ton\n";

  CHECK(!sb.is_error());
  buf.resize(sb.as_cslice().size());
#if TD_WINDOWS
  string new_file_content;
  for (auto c : buf) {
    if (c == '\n') {
      new_file_content += '\r';
    }
    new_file_content += c;
  }
#else
  auto new_file_content = std::move(buf);
#endif
  if (new_file_content != old_file_content.as_slice()) {
    write_file(file_name, new_file_content).ensure();
  }
}

void gen_json_converter(const tl::tl_config &config, const std::string &file_name, const std::string &new_tl_name,
                        Mode mode) {
  tl_name = new_tl_name;

  tl::simple::Schema schema(config);
  gen_json_converter_file(schema, file_name, true, mode);
  gen_json_converter_file(schema, file_name, false, mode);
}

}  // namespace td
