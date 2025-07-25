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

#include "td/tl/tl_config.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <iostream>

namespace td {
namespace tl {
namespace simple {

std::string gen_cpp_name(std::string name) {
  for (std::size_t i = 0; i < name.size(); i++) {
    if ((name[i] < '0' || '9' < name[i]) && (name[i] < 'a' || 'z' < name[i]) && (name[i] < 'A' || 'Z' < name[i])) {
      name[i] = '_';
    }
  }
  assert(name.size() > 0);
  assert(name[name.size() - 1] != '_');
  return name;
}

std::string gen_cpp_field_name(std::string name) {
  return gen_cpp_name(name) + "_";
}

struct CustomType;
struct Type {
  enum {
    Int32,
    Int53,
    Int64,
    Double,
    String,
    Bytes,
    SecureString,
    SecureBytes,
    Vector,
    Bool,
    Int128,
    Int256,
    True,
    Object,
    Function,
    Custom
  } type;

  // type == Custom
  bool is_bare{false};
  const CustomType *custom{nullptr};

  // type == Vector
  const Type *vector_value_type{nullptr};
};

struct Arg {
  const Type *type;
  std::string name;
  int var_num = -1;
  int exist_var_num = -1;
  int exist_var_bit = -1;
};

struct Constructor {
  std::string name;
  std::int32_t id;
  std::vector<Arg> args;
  int var_count = 0;
  const CustomType *type;
};

struct CustomType {
  std::string name;
  std::vector<const Constructor *> constructors;

  mutable bool is_result_{false};
  mutable bool is_query_{false};
};

struct Function {
  std::string name;
  int var_count = 0;
  std::int32_t id;
  std::vector<Arg> args;
  const Type *type;
};

class Schema {
 public:
  explicit Schema(const tl_config &config) {
    config_ = &config;
    for (std::size_t type_num = 0, type_count = config.get_type_count(); type_num < type_count; type_num++) {
      auto *from_type = config.get_type_by_num(type_num);
      if (from_type->name == "Vector") {
        continue;
      }
      auto *type = get_type(from_type);
      if (type->type == Type::Custom) {
        custom_types.push_back(type->custom);
      }
    }
    for (std::size_t function_num = 0, function_count = config.get_function_count(); function_num < function_count;
         function_num++) {
      auto *from_function = config.get_function_by_num(function_num);
      functions.push_back(get_function(from_function));
    }
    for (auto &function : functions_) {
      mark_result(function->type);
      for (auto &arg : function->args) {
        mark_query(arg.type);
      }
    }

    //for (auto custom_type : custom_types) {
    //std::cerr << custom_type->name;
    //if (custom_type->is_result_) {
    //std::cerr << " result";
    //}
    //if (custom_type->is_query_) {
    //std::cerr << " query";
    //}
    //std::cerr << std::endl;
    //}
  }

  std::vector<const CustomType *> custom_types;
  std::vector<const Function *> functions;

 private:
  std::vector<std::unique_ptr<Function>> functions_;
  std::vector<std::unique_ptr<Constructor>> constructors_;
  std::vector<std::unique_ptr<CustomType>> custom_types_;
  std::vector<std::unique_ptr<Type>> types_;

  const tl_config *config_{nullptr};
  std::map<std::int32_t, Type *> type_by_id;
  std::map<std::int32_t, Constructor *> constructor_by_id;
  std::map<std::int32_t, Function *> function_by_id;

  void mark_result(const Type *type) {
    do_mark(type, true);
  }

  void mark_query(const Type *type) {
    do_mark(type, false);
  }

  void do_mark(const Type *type, bool is_result) {
    if (type->type == Type::Vector) {
      return do_mark(type->vector_value_type, is_result);
    }
    if (type->type != Type::Custom) {
      return;
    }
    auto *custom = type->custom;
    auto &was = is_result ? custom->is_result_ : custom->is_query_;
    if (was) {
      return;
    }
    was = true;
    for (auto constructor : custom->constructors) {
      for (auto &arg : constructor->args) {
        do_mark(arg.type, is_result);
      }
    }
  }

  const Type *get_type(const tl_type *from_type) {
    auto &type = type_by_id[from_type->id];
    if (!type) {
      types_.push_back(std::make_unique<Type>());
      type = types_.back().get();

      if (from_type->name == "Int32" || from_type->name == "Int") {
        type->type = Type::Int32;
      } else if (from_type->name == "Int53") {
        type->type = Type::Int53;
      } else if (from_type->name == "Int64" || from_type->name == "Long") {
        type->type = Type::Int64;
      } else if (from_type->name == "Double") {
        type->type = Type::Double;
      } else if (from_type->name == "String") {
        type->type = Type::String;
      } else if (from_type->name == "Bytes") {
        type->type = Type::Bytes;
      } else if (from_type->name == "SecureString") {
        type->type = Type::SecureString;
      } else if (from_type->name == "SecureBytes") {
        type->type = Type::SecureBytes;
      } else if (from_type->name == "Bool") {
        type->type = Type::Bool;
      } else if (from_type->name == "Int128") {
        type->type = Type::Int128;
      } else if (from_type->name == "Int256") {
        type->type = Type::Int256;
      } else if (from_type->name == "True") {
        type->type = Type::True;
      } else if (from_type->name == "Object") {
        type->type = Type::Object;
      } else if (from_type->name == "Function") {
        type->type = Type::Function;
      } else if (from_type->name == "Vector") {
        assert(false);  // unreachable
      } else {
        type->type = Type::Custom;
        custom_types_.push_back(std::make_unique<CustomType>());
        auto *custom_type = custom_types_.back().get();
        type->custom = custom_type;
        custom_type->name = from_type->name;
        for (auto *constructor : from_type->constructors) {
          custom_type->constructors.push_back(get_constructor(constructor));
        }
      }
    }
    return type;
  }

  const CustomType *get_custom_type(const tl_type *from_type) {
    auto *type = get_type(from_type);
    assert(type->type == Type::Custom);
    return type->custom;
  }

  const Constructor *get_constructor(const tl_combinator *from) {
    auto &constructor = constructor_by_id[from->id];
    if (!constructor) {
      constructors_.push_back(std::make_unique<Constructor>());
      constructor = constructors_.back().get();
      constructor->id = from->id;
      constructor->name = from->name;
      constructor->var_count = from->var_count;
      constructor->type = get_custom_type(config_->get_type(from->type_id));
      for (auto &from_arg : from->args) {
        Arg arg;
        arg.name = from_arg.name;
        arg.type = get_type(from_arg.type);
        arg.var_num = from_arg.var_num;
        arg.exist_var_num = from_arg.exist_var_num;
        arg.exist_var_bit = from_arg.exist_var_bit;
        constructor->args.push_back(std::move(arg));
      }
    }
    return constructor;
  }

  const Function *get_function(const tl_combinator *from) {
    auto &function = function_by_id[from->id];
    if (!function) {
      functions_.push_back(std::make_unique<Function>());
      function = functions_.back().get();
      function->id = from->id;
      function->name = from->name;
      function->var_count = from->var_count;
      function->type = get_type(config_->get_type(from->type_id));
      for (auto &from_arg : from->args) {
        Arg arg;
        arg.name = from_arg.name;
        arg.type = get_type(from_arg.type);
        arg.var_num = from_arg.var_num;
        arg.exist_var_num = from_arg.exist_var_num;
        arg.exist_var_bit = from_arg.exist_var_bit;
        function->args.push_back(std::move(arg));
      }
    }
    return function;
  }

  const Type *get_type(const tl_tree *tree) {
    assert(tree->get_type() == NODE_TYPE_TYPE);
    auto *type_tree = static_cast<const tl_tree_type *>(tree);
    if (type_tree->type->name == "Vector") {
      assert(type_tree->children.size() == 1);
      types_.push_back(std::make_unique<Type>());
      auto *type = types_.back().get();
      type->type = Type::Vector;
      type->vector_value_type = get_type(type_tree->children[0]);
      return type;
    } else {
      assert(type_tree->children.empty());
      return get_type(type_tree->type);
    }
  }
};

}  // namespace simple
}  // namespace tl
}  // namespace td
