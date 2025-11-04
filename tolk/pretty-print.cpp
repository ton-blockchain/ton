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
#ifdef TOLK_DEBUG
#include "tolk.h"
#include "ast.h"
#include "ast-stringifier.h"
#include "smart-casts-cfg.h"
#include "generics-helpers.h"

using namespace tolk;

/*
 * This file contains formatters used as LLDB pretty printers.
 * Every function should be named `debug_print` and accept a single const-ref argument.
 * The rest will work automatically.
 * No changes for "prettified" classes are required: these functions are fully standalone.
 * 
 * NOTE! When adding a new function, its argument type should be listed in lldb_addons.py.
 *
 * See .lldbinit and lldb_addons.py.
 */

std::string debug_print(LocalVarPtr var_ref) {
  std::string result = var_ref->is_parameter()
    ? var_ref->is_mutate_parameter() ? "mutate param " : "param "
    : var_ref->is_immutable() ? "val " : "var ";
  if (!var_ref->name.empty()) {
    result += var_ref->name;
  } else {
    result += "_";
  }
  if (var_ref->declared_type) {
    result += ": ";
    result += var_ref->declared_type->as_human_readable();
  }
  return result;
}

std::string debug_print(FunctionPtr sym) {
  std::string result = sym->as_human_readable() + "()";
  if (sym->inferred_return_type) {
    result += ": ";
    result += sym->inferred_return_type->as_human_readable();
  } else if (sym->declared_return_type) {
    result += ": ";
    result += sym->declared_return_type->as_human_readable();
  }
  return result;
}

std::string debug_print(GlobalVarPtr sym) {
  return "global " + sym->name;
}

std::string debug_print(GlobalConstPtr sym) {
  return "const " + sym->name;
}

std::string debug_print(AliasDefPtr sym) {
  return "type " + sym->name;
}

std::string debug_print(StructFieldPtr sym) {
  return "field " + sym->name + ": " + sym->declared_type->as_human_readable();
}

std::string debug_print(StructPtr sym) {
  return "struct " + sym->name;
}

std::string debug_print(EnumMemberPtr sym) {
  return "member " + sym->name;
}

std::string debug_print(EnumDefPtr sym) {
  return "enum " + sym->name;
}

std::string debug_print(const Op* op) {
  std::ostringstream os;

  os << "{enum.Op.cl}";
  switch (op->cl) {
    case Op::_IntConst:
      os << " \'" << op->left[0] << " = ";
      if (op->int_const.is_null()) {
        os << " (null)";
      } else {
        os << op->int_const->to_dec_string();
      }
      break;
    case Op::_SliceConst:
      os << " \'" << op->left[0] << " = " << op->str_const;
      break;
    case Op::_Call:
    case Op::_CallInd:
    case Op::_GlobVar:
    case Op::_SetGlob:
      os << ' ';
      for (size_t i = 0; i < op->left.size(); ++i) {
        if (i)
          os << ',';
        os << '\'' << std::to_string(op->left[i]);
      }
      os << " = ";
      if (op->f_sym != nullptr) {
        os << op->f_sym->name;
        os << '(';
        for (size_t i = 0; i < op->right.size(); ++i) {
          if (i)
            os << ',';
          os << '\'' << op->right[i];
        }
        os << ')';
      }
      if (op->g_sym != nullptr) {
        os << op->g_sym->name;
      }
      break;
    case Op::_Let:
    case Op::_Tuple:
    case Op::_UnTuple:
    case Op::_Return:
    case Op::_Import:
      os << " ";
      for (size_t i = 0; i < op->left.size(); ++i) {
        if (i)
          os << ',';
        os << '\'' << op->left[i];
      }
      if (!op->right.empty()) {
        os << " = ";
        for (size_t i = 0; i < op->right.size(); ++i) {
          if (i)
            os << ',';
          os << '\'' << op->right[i];
        }
      }
      break;
    default:
      break;
  }

  if (op->flags & Op::_Disabled)
    os << " |disabled";
  if (op->flags & Op::_NoReturn)
    os << " |noret";
  if (op->flags & Op::_Impure)
    os << " |impure";

  return os.str();
}

std::string debug_print(TypePtr e_type) {
  return e_type->as_human_readable();
}

std::string debug_print(const VarDescr* v) {
  std::ostringstream os;

  if (v->flags & VarDescr::_Last) {
    os << '*';
  }
  if (v->flags & VarDescr::_Unused) {
    os << '?';
  }
  os << '\'' << v->idx << ':';
  v->show_value(os);

  return os.str();
}

std::string debug_print(const TmpVar* v) {
  std::ostringstream os;

  os << '\'' << v->ir_idx;
  if (!v->name.empty()) {
    os << '_' << v->name;
  }
  if (v->v_type) {
    os << " <" << v->v_type->as_human_readable() << '>';
  }
  if (v->purpose) {
    os << ' ' << v->purpose;
  }

  return os.str();
}

std::string debug_print(const VarDescrList* vd) {
  std::ostringstream os;

  os << vd->size();
  for (size_t i = 0; i < vd->list.size(); ++i) {
    if (i == 0) {
      os << ": ";
    } else {
      os << ", ";
    }

    if (vd->list[i].flags & VarDescr::_Last) {
      os << '*';
    }
    if (vd->list[i].flags & VarDescr::_Unused) {
      os << '?';
    }
    os << '\'' << vd->list[i].idx;
  }

  return os.str();
}

std::string debug_print(const AsmOp* op) {
  std::ostringstream os;

  os << "{enum.AsmOp.t} " << op->op;
  return os.str();
}

std::string debug_print(const AsmOpList* op_list) {
  int n_stmt = 0, n_comments = 0;
  for (const AsmOp& cur : op_list->list_) {
    if (cur.is_comment()) {
      n_comments++;
    } else {
      n_stmt++;
    }
  }

  std::ostringstream os;

  os << "n_stmt=" << n_stmt << ", "
     << "n_comments=" << n_comments;
  return os.str();
}

std::string debug_print(const Stack* stack) {
  std::ostringstream os;
  
  if (stack->s.empty()) {
    os << "(empty) ";
  } else {
    os << stack->s.size() << ": ";
    for (const auto &[var_idx, const_idx] : stack->s) {
      if (var_idx != -1) {
        const TmpVar& v = stack->o.var_names_->at(var_idx);
        os << '\'' << v.ir_idx;
        if (!v.name.empty()) {
          os << ' ' << v.name;
        }
        if (v.purpose) {
          os << ' ' << v.purpose;
        }
      }
      if (const_idx != -1) {
        os << '=' << stack->o.constants_[const_idx]->to_dec_string();
      }
      os << ' ';
    }
  }

  if (!(stack->mode & Stack::_Shown)) {
    os << " !_Shown";
  }
  return os.str();
}

std::string debug_print(const Token* token) {
  return static_cast<std::string>(token->str_val);
}

std::string debug_print(const SrcRange* range) {
  return range->stringify_start_location(true);
}

std::string debug_print(const FlowContext* flow) {
  std::ostringstream os;
  os << *flow;
  return os.str();
}

std::string debug_print(const SinkExpression* s_expr) {
  return s_expr->to_string();
}

std::string debug_print(const FactsAboutExpr* info) {
  std::ostringstream os;
  os << *info;
  return os.str();
}

std::string debug_print(const GenericsDeclaration* genericTs) {
  return genericTs->as_human_readable();
}

std::string debug_print(const GenericsSubstitutions* substitutedTs) {
  return substitutedTs->as_human_readable(true);
}

std::string debug_print(AnyV v) {
  return ASTStringifier::to_string_without_children(v);
}

#endif  // TOLK_DEBUG
