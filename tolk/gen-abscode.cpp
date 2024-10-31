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

using namespace std::literals::string_literals;

namespace tolk {

/*
 * 
 *   EXPRESSIONS
 * 
 */

Expr* Expr::copy() const {
  auto res = new Expr{*this};
  for (auto& arg : res->args) {
    arg = arg->copy();
  }
  return res;
}

Expr::Expr(ExprCls c, sym_idx_t name_idx, std::initializer_list<Expr*> _arglist) : cls(c), args(std::move(_arglist)) {
  sym = lookup_symbol(name_idx);
  if (!sym) {
  }
}

void Expr::deduce_type() {
  if (e_type) {
    return;
  }
  switch (cls) {
    case _Apply: {
      if (!sym) {
        return;
      }
      SymValFunc* sym_val = dynamic_cast<SymValFunc*>(sym->value);
      if (!sym_val || !sym_val->get_type()) {
        return;
      }
      std::vector<TypeExpr*> arg_types;
      arg_types.reserve(args.size());
      for (const Expr* arg : args) {
        arg_types.push_back(arg->e_type);
      }
      TypeExpr* fun_type = TypeExpr::new_map(TypeExpr::new_tensor(arg_types), TypeExpr::new_hole());
      try {
        unify(fun_type, sym_val->sym_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot apply function " << sym->name() << " : " << sym_val->get_type() << " to arguments of type "
           << fun_type->args[0] << ": " << ue;
        throw ParseError(here, os.str());
      }
      e_type = fun_type->args[1];
      TypeExpr::remove_indirect(e_type);
      return;
    }
    case _VarApply: {
      tolk_assert(args.size() == 2);
      TypeExpr* fun_type = TypeExpr::new_map(args[1]->e_type, TypeExpr::new_hole());
      try {
        unify(fun_type, args[0]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot apply expression of type " << args[0]->e_type << " to an expression of type " << args[1]->e_type
           << ": " << ue;
        throw ParseError(here, os.str());
      }
      e_type = fun_type->args[1];
      TypeExpr::remove_indirect(e_type);
      return;
    }
    case _GrabMutatedVars: {
      tolk_assert(args.size() == 2 && args[0]->cls == _Apply && sym);
      SymValFunc* called_f = dynamic_cast<SymValFunc*>(sym->value);
      tolk_assert(called_f->has_mutate_params());
      TypeExpr* sym_type = called_f->get_type();
      if (sym_type->constr == TypeExpr::te_ForAll) {
        TypeExpr::remove_forall(sym_type);
      }
      tolk_assert(sym_type->args[1]->constr == TypeExpr::te_Tensor);
      e_type = sym_type->args[1]->args[sym_type->args[1]->args.size() - 1];
      TypeExpr::remove_indirect(e_type);
      return;
    }
    case _ReturnSelf: {
      tolk_assert(args.size() == 2 && sym);
      Expr* this_arg = args[1];
      e_type = this_arg->e_type;
      TypeExpr::remove_indirect(e_type);
      return;
    }
    case _Letop: {
      tolk_assert(args.size() == 2);
      try {
        // std::cerr << "in assignment: " << args[0]->e_type << " from " << args[1]->e_type << std::endl;
        unify(args[0]->e_type, args[1]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot assign an expression of type " << args[1]->e_type << " to a variable or pattern of type "
           << args[0]->e_type << ": " << ue;
        throw ParseError(here, os.str());
      }
      e_type = args[0]->e_type;
      TypeExpr::remove_indirect(e_type);
      return;
    }
    case _CondExpr: {
      tolk_assert(args.size() == 3);
      auto flag_type = TypeExpr::new_atomic(TypeExpr::_Int);
      try {
        unify(args[0]->e_type, flag_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "condition in a conditional expression has non-integer type " << args[0]->e_type << ": " << ue;
        throw ParseError(here, os.str());
      }
      try {
        unify(args[1]->e_type, args[2]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "the two variants in a conditional expression have different types " << args[1]->e_type << " and "
           << args[2]->e_type << " : " << ue;
        throw ParseError(here, os.str());
      }
      e_type = args[1]->e_type;
      TypeExpr::remove_indirect(e_type);
      return;
    }
    default:
      throw Fatal("unexpected cls=" + std::to_string(cls) + " in Expr::deduce_type()");
  }
}

void Expr::define_new_vars(CodeBlob& code) {
  switch (cls) {
    case _Tensor:
    case _MkTuple: {
      for (Expr* item : args) {
        item->define_new_vars(code);
      }
      break;
    }
    case _Var:
      if (val < 0) {
        val = code.create_var(e_type, sym->sym_idx, here);
        sym->value->idx = val;
      }
      break;
    case _Hole:
      if (val < 0) {
        val = code.create_tmp_var(e_type, here);
      }
      break;
    default:
      break;
  }
}

void Expr::predefine_vars() {
  switch (cls) {
    case _Tensor:
    case _MkTuple: {
      for (Expr* item : args) {
        item->predefine_vars();
      }
      break;
    }
    case _Var:
      if (!sym) {
        tolk_assert(val < 0 && here.is_defined());
        sym = define_symbol(~val, false, here);
        // std::cerr << "predefining variable " << symbols.get_name(~val) << std::endl;
        if (!sym) {
          throw ParseError{here, std::string{"redefined variable `"} + G.symbols.get_name(~val) + "`"};
        }
        sym->value = new SymValVariable(-1, e_type);
        if (is_immutable()) {
          dynamic_cast<SymValVariable*>(sym->value)->flags |= SymValVariable::flagImmutable;
        }
      }
      break;
    default:
      break;
  }
}

var_idx_t Expr::new_tmp(CodeBlob& code) const {
  return code.create_tmp_var(e_type, here);
}

void add_set_globs(CodeBlob& code, std::vector<std::pair<SymDef*, var_idx_t>>& globs, SrcLocation here) {
  for (const auto& p : globs) {
    auto& op = code.emplace_back(here, Op::_SetGlob, std::vector<var_idx_t>{}, std::vector<var_idx_t>{ p.second }, p.first);
    op.set_impure(code);
  }
}

std::vector<var_idx_t> pre_compile_let(CodeBlob& code, Expr* lhs, Expr* rhs, SrcLocation here) {
  if (lhs->is_mktuple()) {
    if (rhs->is_mktuple()) {
      return pre_compile_let(code, lhs->args.at(0), rhs->args.at(0), here);
    }
    auto right = rhs->pre_compile(code);
    TypeExpr::remove_indirect(rhs->e_type);
    auto unpacked_type = rhs->e_type->args.at(0);
    std::vector<var_idx_t> tmp{code.create_tmp_var(unpacked_type, rhs->here)};
    code.emplace_back(lhs->here, Op::_UnTuple, tmp, std::move(right));
    auto tvar = new Expr{Expr::_Var, lhs->here};
    tvar->set_val(tmp[0]);
    tvar->set_location(rhs->here);
    tvar->e_type = unpacked_type;
    pre_compile_let(code, lhs->args.at(0), tvar, here);
    return tmp;
  }
  auto right = rhs->pre_compile(code);
  std::vector<std::pair<SymDef*, var_idx_t>> globs;
  auto left = lhs->pre_compile(code, &globs);
  for (var_idx_t v : left) {
    code.on_var_modification(v, here);
  }
  code.emplace_back(here, Op::_Let, std::move(left), right);
  add_set_globs(code, globs, here);
  return right;
}

std::vector<var_idx_t> pre_compile_tensor(const std::vector<Expr *>& args, CodeBlob &code,
                                          std::vector<std::pair<SymDef*, var_idx_t>> *lval_globs) {
  const size_t n = args.size();
  if (n == 0) {  // just `()`
    return {};
  }
  if (n == 1) {  // just `(x)`: even if x is modified (e.g. `f(x=x+2)`), there are no next arguments
    return args[0]->pre_compile(code, lval_globs);
  }
  std::vector<std::vector<var_idx_t>> res_lists(n);

  struct ModifiedVar {
    size_t i, j;
    std::unique_ptr<Op>* cur_ops; // `LET tmp = v_ij` will be inserted before this
  };
  std::vector<ModifiedVar> modified_vars;
  for (size_t i = 0; i < n; ++i) {
    res_lists[i] = args[i]->pre_compile(code, lval_globs);
    for (size_t j = 0; j < res_lists[i].size(); ++j) {
      TmpVar& var = code.vars.at(res_lists[i][j]);
      if (!lval_globs && !var.is_unnamed()) {
        var.on_modification.push_back([&modified_vars, i, j, cur_ops = code.cur_ops, done = false](SrcLocation here) mutable {
          if (!done) {
            done = true;
            modified_vars.push_back({i, j, cur_ops});
          }
        });
      } else {
        var.on_modification.push_back([](SrcLocation) {
        });
      }
    }
  }
  for (const auto& list : res_lists) {
    for (var_idx_t v : list) {
      tolk_assert(!code.vars.at(v).on_modification.empty());
      code.vars.at(v).on_modification.pop_back();
    }
  }
  for (size_t idx = modified_vars.size(); idx--; ) {
    const ModifiedVar &m = modified_vars[idx];
    var_idx_t orig_v = res_lists[m.i][m.j];
    var_idx_t tmp_v = code.create_tmp_var(code.vars[orig_v].v_type, code.vars[orig_v].where);
    std::unique_ptr<Op> op = std::make_unique<Op>(code.vars[orig_v].where, Op::_Let);
    op->left = {tmp_v};
    op->right = {orig_v};
    op->next = std::move((*m.cur_ops));
    *m.cur_ops = std::move(op);
    res_lists[m.i][m.j] = tmp_v;
  }
  std::vector<var_idx_t> res;
  for (const auto& list : res_lists) {
    res.insert(res.end(), list.cbegin(), list.cend());
  }
  return res;
}

std::vector<var_idx_t> Expr::pre_compile(CodeBlob& code, std::vector<std::pair<SymDef*, var_idx_t>>* lval_globs) const {
  if (lval_globs && !(cls == _Tensor || cls == _Var || cls == _Hole || cls == _GlobVar)) {
    std::cerr << "lvalue expression constructor is " << cls << std::endl;
    throw Fatal{"cannot compile lvalue expression with unknown constructor"};
  }
  switch (cls) {
    case _Tensor: {
      return pre_compile_tensor(args, code, lval_globs);
    }
    case _Apply: {
      tolk_assert(sym);
      std::vector<var_idx_t> res = pre_compile_tensor(args, code, lval_globs);;
      auto rvect = new_tmp_vect(code);
      auto& op = code.emplace_back(here, Op::_Call, rvect, res, sym);
      if (flags & _IsImpure) {
        op.set_impure(code);
      }
      return rvect;
    }
    case _GrabMutatedVars: {
      SymValFunc* func_val = dynamic_cast<SymValFunc*>(sym->value);
      tolk_assert(func_val && func_val->has_mutate_params());
      tolk_assert(args.size() == 2 && args[0]->cls == _Apply && args[1]->cls == _Tensor);
      auto right = args[0]->pre_compile(code);    // apply (returning function result and mutated)
      std::vector<std::pair<SymDef*, var_idx_t>> local_globs;
      if (!lval_globs) {
        lval_globs = &local_globs;
      }
      auto left = args[1]->pre_compile(code, lval_globs);   // mutated (lvalue)
      auto rvect = new_tmp_vect(code);
      left.push_back(rvect[0]);
      for (var_idx_t v : left) {
        code.on_var_modification(v, here);
      }
      code.emplace_back(here, Op::_Let, std::move(left), std::move(right));
      add_set_globs(code, local_globs, here);
      return rvect;
    }
    case _ReturnSelf: {
      tolk_assert(args.size() == 2 && sym);
      Expr* this_arg = args[1];
      auto right = args[0]->pre_compile(code);
      return this_arg->pre_compile(code);
    }
    case _Var:
    case _Hole:
      if (val < 0) {
        throw ParseError{here, "unexpected variable definition"};
      }
      return {val};
    case _VarApply:
      if (args[0]->cls == _GlobFunc) {
        auto res = args[1]->pre_compile(code);
        auto rvect = new_tmp_vect(code);
        auto& op = code.emplace_back(here, Op::_Call, rvect, std::move(res), args[0]->sym);
        if (args[0]->flags & _IsImpure) {
          op.set_impure(code);
        }
        return rvect;
      } else {
        auto res = args[1]->pre_compile(code);
        auto tfunc = args[0]->pre_compile(code);
        if (tfunc.size() != 1) {
          throw Fatal{"stack tuple used as a function"};
        }
        res.push_back(tfunc[0]);
        auto rvect = new_tmp_vect(code);
        code.emplace_back(here, Op::_CallInd, rvect, std::move(res));
        return rvect;
      }
    case _Const: {
      auto rvect = new_tmp_vect(code);
      code.emplace_back(here, Op::_IntConst, rvect, intval);
      return rvect;
    }
    case _GlobFunc:
    case _GlobVar: {
      if (auto fun_ref = dynamic_cast<SymValFunc*>(sym->value)) {
        fun_ref->flags |= SymValFunc::flagUsedAsNonCall;
        if (!fun_ref->arg_order.empty() || !fun_ref->ret_order.empty()) {
          throw ParseError(here, "saving `" + sym->name() + "` into a variable will most likely lead to invalid usage, since it changes the order of variables on the stack");
        }
        if (fun_ref->has_mutate_params()) {
          throw ParseError(here, "saving `" + sym->name() + "` into a variable is impossible, since it has `mutate` parameters and thus can only be called directly");
        }
      }
      auto rvect = new_tmp_vect(code);
      if (lval_globs) {
        lval_globs->push_back({ sym, rvect[0] });
        return rvect;
      } else {
        code.emplace_back(here, Op::_GlobVar, rvect, std::vector<var_idx_t>{}, sym);
        return rvect;
      }
    }
    case _Letop: {
      return pre_compile_let(code, args.at(0), args.at(1), here);
    }
    case _MkTuple: {
      auto left = new_tmp_vect(code);
      auto right = args[0]->pre_compile(code);
      code.emplace_back(here, Op::_Tuple, left, std::move(right));
      return left;
    }
    case _CondExpr: {
      auto cond = args[0]->pre_compile(code);
      tolk_assert(cond.size() == 1);
      auto rvect = new_tmp_vect(code);
      Op& if_op = code.emplace_back(here, Op::_If, cond);
      code.push_set_cur(if_op.block0);
      code.emplace_back(here, Op::_Let, rvect, args[1]->pre_compile(code));
      code.close_pop_cur(args[1]->here);
      code.push_set_cur(if_op.block1);
      code.emplace_back(here, Op::_Let, rvect, args[2]->pre_compile(code));
      code.close_pop_cur(args[2]->here);
      return rvect;
    }
    case _SliceConst: {
      auto rvect = new_tmp_vect(code);
      code.emplace_back(here, Op::_SliceConst, rvect, strval);
      return rvect;
    }
    default:
      std::cerr << "expression constructor is " << cls << std::endl;
      throw Fatal{"cannot compile expression with unknown constructor"};
  }
}

}  // namespace tolk
