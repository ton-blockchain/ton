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

#include "src-file.h"
#include "type-expr.h"
#include <functional>
#include <memory>

namespace tolk {

typedef int var_idx_t;
typedef int sym_idx_t;

enum class SymValKind { _Var, _Func, _GlobVar, _Const };

struct SymValBase {
  SymValKind kind;
  int idx;
  TypeExpr* sym_type;
#ifdef TOLK_DEBUG
  std::string sym_name; // seeing symbol name in debugger makes it much easier to delve into Tolk sources
#endif

  SymValBase(SymValKind kind, int idx, TypeExpr* sym_type) : kind(kind), idx(idx), sym_type(sym_type) {
  }
  virtual ~SymValBase() = default;

  TypeExpr* get_type() const {
    return sym_type;
  }
};


struct Symbol {
  std::string str;
  sym_idx_t idx;

  Symbol(std::string str, sym_idx_t idx) : str(std::move(str)), idx(idx) {}

  static std::string unknown_symbol_name(sym_idx_t i);
};

class SymTable {
public:
  static constexpr int SIZE_PRIME = 100003;

private:
  sym_idx_t def_sym{0};
  std::unique_ptr<Symbol> sym[SIZE_PRIME + 1];
  sym_idx_t gen_lookup(std::string_view str, int mode = 0, sym_idx_t idx = 0);

public:

  static constexpr sym_idx_t not_found = 0;
  sym_idx_t lookup(std::string_view str) {
    return gen_lookup(str, 0);
  }
  sym_idx_t lookup_add(std::string_view str) {
    return gen_lookup(str, 1);
  }
  Symbol* operator[](sym_idx_t i) const {
    return sym[i].get();
  }
  std::string get_name(sym_idx_t i) const {
    return sym[i] ? sym[i]->str : Symbol::unknown_symbol_name(i);
  }
};

struct SymTableOverflow {
  int sym_def;
  explicit SymTableOverflow(int x) : sym_def(x) {
  }
};


struct SymDef {
  int level;
  sym_idx_t sym_idx;
  SymValBase* value;
  SrcLocation loc;
#ifdef TOLK_DEBUG
  std::string sym_name;
#endif
  SymDef(int lvl, sym_idx_t idx, SrcLocation _loc, SymValBase* val = nullptr)
      : level(lvl), sym_idx(idx), value(val), loc(_loc) {
  }
  std::string name() const;
};


void open_scope(SrcLocation loc);
void close_scope();
SymDef* lookup_symbol(sym_idx_t idx);

SymDef* define_global_symbol(sym_idx_t name_idx, SrcLocation loc = {});
SymDef* define_parameter(sym_idx_t name_idx, SrcLocation loc);
SymDef* define_symbol(sym_idx_t name_idx, bool force_new, SrcLocation loc);

}  // namespace tolk
