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
#include "tolk.h"
#include "compiler-state.h"
#include "type-system.h"

namespace tolk {

/*
 * 
 *   ABSTRACT CODE
 * 
 */

void TmpVar::show_as_stack_comment(std::ostream& os) const {
  if (!name.empty()) {
    os << name;
  } else {
    os << '\'' << ir_idx;
  }
#ifdef TOLK_DEBUG
  // uncomment for detailed stack output, like `'15(binary-op) '16(glob-var)`
  // if (desc) os << desc;
#endif
}

void TmpVar::show(std::ostream& os) const {
  os << '\'' << ir_idx;   // vars are printed out as `'1 '2` (in stack comments, debug info, etc.)
  if (!name.empty()) {
    os << '_' << name;
  }
#ifdef TOLK_DEBUG
  if (desc) {
    os << ' ' << desc;    // "origin" of implicitly created tmp var, like `'15 (binary-op) '16 (glob-var)`
  }
#endif
}

std::ostream& operator<<(std::ostream& os, const TmpVar& var) {
  var.show(os);
  return os;
}

void VarDescr::show_value(std::ostream& os) const {
  if (val & _Int) {
    os << 'i';
  }
  if (val & _Zero) {
    os << '0';
  }
  if (val & _NonZero) {
    os << '!';
  }
  if (val & _Pos) {
    os << '>';
  }
  if (val & _Neg) {
    os << '<';
  }
  if (val & _Even) {
    os << 'E';
  }
  if (val & _Odd) {
    os << 'O';
  }
  if (val & _Finite) {
    os << 'f';
  }
  if (val & _Nan) {
    os << 'N';
  }
  if (int_const.not_null()) {
    os << '=' << int_const;
  }
}

void VarDescr::show(std::ostream& os, const char* name) const {
  if (flags & _Last) {
    os << '*';
  }
  if (flags & _Unused) {
    os << '?';
  }
  if (name) {
    os << name;
  }
  os << '\'' << idx;
  show_value(os);
}

void VarDescr::set_const(long long value) {
  return set_const(td::make_refint(value));
}

void VarDescr::set_const(td::RefInt256 value) {
  int_const = std::move(value);
  if (!int_const->signed_fits_bits(257)) {
    int_const.write().invalidate();
  }
  val = _Int;
  int s = sgn(int_const);
  if (s < -1) {
    val |= _Nan | _NonZero;
  } else if (s < 0) {
    val |= _NonZero | _Neg | _Finite;
  } else if (s > 0) {
    val |= _NonZero | _Pos | _Finite;
  } else {
    val |= _Zero | _Neg | _Pos | _Finite;
  }
  if (val & _Finite) {
    val |= int_const->get_bit(0) ? _Odd : _Even;
  }
}

void VarDescr::set_const(const std::string&) {
  int_const.clear();
  val = 0;
}

void VarDescr::operator|=(const VarDescr& y) {
  if (is_int_const()) {
    bool y_same = y.is_int_const() && *int_const == *y.int_const;
    if (!y_same) {
      int_const.clear();
    }
  }
  val &= y.val;
}

void VarDescr::operator&=(const VarDescr& y) {
  if (y.is_int_const()) {
    int_const = y.int_const;
  }
  val |= y.val;
}

void VarDescr::set_value(const VarDescr& y) {
  int_const = y.int_const;
  val = y.val;
}

void VarDescr::set_value(VarDescr&& y) {
  int_const = std::move(y.int_const);
  val = y.val;
}

void VarDescr::clear_value() {
  int_const.clear();
  val = 0;
}

void VarDescrList::show(std::ostream& os) const {
  if (unreachable) {
    os << "<unreachable> ";
  }
  os << "[";
  for (const auto& v : list) {
    os << ' ' << v;
  }
  os << " ]\n";
}

void Op::show(std::ostream& os, const std::vector<TmpVar>& vars, std::string pfx, int mode) const {
  if (mode & 2) {
    os << pfx << " [";
    for (const auto& v : var_info.list) {
      os << ' ';
      if (v.flags & VarDescr::_Last) {
        os << '*';
      }
      if (v.flags & VarDescr::_Unused) {
        os << '?';
      }
      os << vars[v.idx];
      if (mode & 4) {
        os << ':';
        v.show_value(os);
      }
    }
    os << " ]\n";
  }
  std::string dis = disabled() ? "<disabled> " : "";
  if (noreturn()) {
    dis += "<noret> ";
  }
  if (impure()) {
    dis += "<impure> ";
  }
  switch (cl) {
    case _Nop:
      os << pfx << dis << "NOP\n";
      break;
    case _Call:
      os << pfx << dis << "CALL: ";
      show_var_list(os, left, vars);
      os << " := " << (f_sym ? f_sym->name : "(null)") << " ";
      if ((mode & 4) && args.size() == right.size()) {
        show_var_list(os, args, vars);
      } else {
        show_var_list(os, right, vars);
      }
      os << std::endl;
      break;
    case _CallInd:
      os << pfx << dis << "CALLIND: ";
      show_var_list(os, left, vars);
      os << " := EXEC ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
    case _Let:
      os << pfx << dis << "LET ";
      show_var_list(os, left, vars);
      os << " := ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
    case _Tuple:
      os << pfx << dis << "MKTUPLE ";
      show_var_list(os, left, vars);
      os << " := ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
    case _UnTuple:
      os << pfx << dis << "UNTUPLE ";
      show_var_list(os, left, vars);
      os << " := ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
    case _IntConst:
      os << pfx << dis << "CONST ";
      show_var_list(os, left, vars);
      os << " := " << int_const << std::endl;
      break;
    case _SliceConst:
      os << pfx << dis << "SCONST ";
      show_var_list(os, left, vars);
      os << " := " << str_const << std::endl;
      break;
    case _Import:
      os << pfx << dis << "IMPORT ";
      show_var_list(os, left, vars);
      os << std::endl;
      break;
    case _Return:
      os << pfx << dis << "RETURN ";
      show_var_list(os, left, vars);
      os << std::endl;
      break;
    case _GlobVar:
      os << pfx << dis << "GLOBVAR ";
      show_var_list(os, left, vars);
      os << " := " << (g_sym ? g_sym->name : "(null)") << std::endl;
      break;
    case _SetGlob:
      os << pfx << dis << "SETGLOB ";
      os << (g_sym ? g_sym->name : "(null)") << " := ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
    case _Repeat:
      os << pfx << dis << "REPEAT ";
      show_var_list(os, left, vars);
      os << ' ';
      show_block(os, block0.get(), vars, pfx, mode);
      os << std::endl;
      break;
    case _If:
      os << pfx << dis << "IF ";
      show_var_list(os, left, vars);
      os << ' ';
      show_block(os, block0.get(), vars, pfx, mode);
      os << " ELSE ";
      show_block(os, block1.get(), vars, pfx, mode);
      os << std::endl;
      break;
    case _While:
      os << pfx << dis << "WHILE ";
      show_var_list(os, left, vars);
      os << ' ';
      show_block(os, block0.get(), vars, pfx, mode);
      os << " DO ";
      show_block(os, block1.get(), vars, pfx, mode);
      os << std::endl;
      break;
    case _Until:
      os << pfx << dis << "UNTIL ";
      show_var_list(os, left, vars);
      os << ' ';
      show_block(os, block0.get(), vars, pfx, mode);
      os << std::endl;
      break;
    case _Again:
      os << pfx << dis << "AGAIN ";
      show_var_list(os, left, vars);
      os << ' ';
      show_block(os, block0.get(), vars, pfx, mode);
      os << std::endl;
      break;
    default:
      os << pfx << dis << "<???" << cl << "> ";
      show_var_list(os, left, vars);
      os << " -- ";
      show_var_list(os, right, vars);
      os << std::endl;
      break;
  }
}

void Op::show_var_list(std::ostream& os, const std::vector<var_idx_t>& idx_list,
                       const std::vector<TmpVar>& vars) const {
  if (!idx_list.size()) {
    os << "()";
  } else if (idx_list.size() == 1) {
    os << vars.at(idx_list[0]);
  } else {
    os << "(" << vars.at(idx_list[0]);
    for (std::size_t i = 1; i < idx_list.size(); i++) {
      os << ", " << vars.at(idx_list[i]);
    }
    os << ")";
  }
}

void Op::show_var_list(std::ostream& os, const std::vector<VarDescr>& list, const std::vector<TmpVar>& vars) const {
  auto n = list.size();
  if (!n) {
    os << "()";
  } else {
    os << "( ";
    for (std::size_t i = 0; i < list.size(); i++) {
      if (i) {
        os << ", ";
      }
      if (list[i].is_unused()) {
        os << '?';
      }
      os << vars.at(list[i].idx) << ':';
      list[i].show_value(os);
    }
    os << " )";
  }
}

void Op::show_block(std::ostream& os, const Op* block, const std::vector<TmpVar>& vars, std::string pfx, int mode) {
  os << "{" << std::endl;
  std::string pfx2 = pfx + "  ";
  for (const Op& op : block) {
    op.show(os, vars, pfx2, mode);
  }
  os << pfx << "}";
}

std::ostream& operator<<(std::ostream& os, const CodeBlob& code) {
  code.print(os);
  return os;
}

// flags: +1 = show variable definition locations; +2 = show vars after each op; +4 = show var abstract value info after each op; +8 = show all variables at start
void CodeBlob::print(std::ostream& os, int flags) const {
  os << "CODE BLOB: " << var_cnt << " variables, " << in_var_cnt << " input\n";
  if ((flags & 8) != 0) {
    for (const auto& var : vars) {
      var.show(os);
      os << " : " << var.v_type << std::endl;
      if (var.loc.is_defined() && (flags & 1) != 0) {
        var.loc.show(os);
        os << " defined here:\n";
        var.loc.show_context(os);
      }
    }
  }
  os << "------- BEGIN --------\n";
  for (const auto& op : ops) {
    op.show(os, vars, "", flags);
  }
  os << "-------- END ---------\n\n";
}

std::vector<var_idx_t> CodeBlob::create_var(TypePtr var_type, SrcLocation loc, std::string name) {
  std::vector<var_idx_t> ir_idx;
  int stack_w = var_type->get_width_on_stack();
  ir_idx.reserve(stack_w);
  if (const TypeDataStruct* t_struct = var_type->try_as<TypeDataStruct>()) {
    for (int i = 0; i < t_struct->struct_ref->get_num_fields(); ++i) {
      StructFieldPtr field_ref = t_struct->struct_ref->get_field(i);
      std::string sub_name = name.empty() || t_struct->struct_ref->get_num_fields() == 1 ? name : name + "." + field_ref->name;
      std::vector<var_idx_t> nested = create_var(field_ref->declared_type, loc, std::move(sub_name));
      ir_idx.insert(ir_idx.end(), nested.begin(), nested.end());
    }
  } else if (const TypeDataTensor* t_tensor = var_type->try_as<TypeDataTensor>()) {
    for (int i = 0; i < t_tensor->size(); ++i) {
      std::string sub_name = name.empty() ? name : name + "." + std::to_string(i);
      std::vector<var_idx_t> nested = create_var(t_tensor->items[i], loc, std::move(sub_name));
      ir_idx.insert(ir_idx.end(), nested.begin(), nested.end());
    }
  } else if (const TypeDataAlias* t_alias = var_type->try_as<TypeDataAlias>()) {
    ir_idx = create_var(t_alias->underlying_type, loc, std::move(name));
  } else if (const TypeDataUnion* t_union = var_type->try_as<TypeDataUnion>(); t_union && stack_w != 1) {
    std::string utag_name = name.empty() ? "'UTag" : name + ".UTag";
    if (t_union->or_null) {   // in stack comments, `a:(int,int)?` will be "a.0 a.1 a.UTag"
      ir_idx = create_var(t_union->or_null, loc, std::move(name));
    } else {                  // in stack comments, `a:int|slice` will be "a.USlot1 a.UTag"
      for (int i = 0; i < stack_w - 1; ++i) {
        std::string slot_name = name.empty() ? "'USlot" + std::to_string(i + 1) : name + ".USlot" + std::to_string(i + 1);
        ir_idx.emplace_back(create_var(TypeDataUnknown::create(), loc, std::move(slot_name))[0]);
      }
    }
    ir_idx.emplace_back(create_var(TypeDataInt::create(), loc, std::move(utag_name))[0]);
  } else if (var_type != TypeDataVoid::create() && var_type != TypeDataNever::create()) {
#ifdef TOLK_DEBUG
    tolk_assert(stack_w == 1);
#endif
    vars.emplace_back(var_cnt, var_type, std::move(name), loc);
    ir_idx.emplace_back(var_cnt);
    var_cnt++;
  }
  tolk_assert(static_cast<int>(ir_idx.size()) == stack_w);
  return ir_idx;
}

var_idx_t CodeBlob::create_int(SrcLocation loc, int64_t value, const char* desc) {
  vars.emplace_back(var_cnt, TypeDataInt::create(), std::string{}, loc);
#ifdef TOLK_DEBUG
  vars.back().desc = desc;
#endif
  var_idx_t ir_int = var_cnt;
  var_cnt++;
  emplace_back(loc, Op::_IntConst, std::vector{ir_int}, td::make_refint(value));
  return ir_int;
}

}  // namespace tolk
