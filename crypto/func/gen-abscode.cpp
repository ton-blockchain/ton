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
#include <numeric>
#include "func.h"

using namespace std::literals::string_literals;

namespace funC {

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
  sym = sym::lookup_symbol(name_idx);
  if (!sym) {
  }
}

void Expr::chk_rvalue(const Lexem& lem) const {
  if (!is_rvalue()) {
    lem.error_at("rvalue expected before `", "`");
  }
}

void Expr::chk_lvalue(const Lexem& lem) const {
  if (!is_lvalue()) {
    lem.error_at("lvalue expected before `", "`");
  }
}

void Expr::chk_type(const Lexem& lem) const {
  if (!is_type()) {
    lem.error_at("type expression expected before `", "`");
  }
}

bool Expr::deduce_type(const Lexem& lem) {
  if (e_type) {
    return true;
  }
  switch (cls) {
    case _Apply: {
      if (!sym) {
        return false;
      }
      SymVal* sym_val = dynamic_cast<SymVal*>(sym->value);
      if (!sym_val || !sym_val->get_type()) {
        return false;
      }
      std::vector<TypeExpr*> arg_types;
      for (const auto& arg : args) {
        arg_types.push_back(arg->e_type);
      }
      TypeExpr* fun_type = TypeExpr::new_map(TypeExpr::new_tensor(arg_types), TypeExpr::new_hole());
      try {
        unify(fun_type, sym_val->sym_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot apply function " << sym->name() << " : " << sym_val->get_type() << " to arguments of type "
           << fun_type->args[0] << ": " << ue;
        lem.error(os.str());
      }
      e_type = fun_type->args[1];
      TypeExpr::remove_indirect(e_type);
      return true;
    }
    case _VarApply: {
      func_assert(args.size() == 2);
      TypeExpr* fun_type = TypeExpr::new_map(args[1]->e_type, TypeExpr::new_hole());
      try {
        unify(fun_type, args[0]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot apply expression of type " << args[0]->e_type << " to an expression of type " << args[1]->e_type
           << ": " << ue;
        lem.error(os.str());
      }
      e_type = fun_type->args[1];
      TypeExpr::remove_indirect(e_type);
      return true;
    }
    case _Letop: {
      func_assert(args.size() == 2);
      try {
        // std::cerr << "in assignment: " << args[0]->e_type << " from " << args[1]->e_type << std::endl;
        unify(args[0]->e_type, args[1]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot assign an expression of type " << args[1]->e_type << " to a variable or pattern of type "
           << args[0]->e_type << ": " << ue;
        lem.error(os.str());
      }
      e_type = args[0]->e_type;
      TypeExpr::remove_indirect(e_type);
      return true;
    }
    case _LetFirst: {
      func_assert(args.size() == 2);
      TypeExpr* rhs_type = TypeExpr::new_tensor({args[0]->e_type, TypeExpr::new_hole()});
      try {
        // std::cerr << "in implicit assignment of a modifying method: " << rhs_type << " and " << args[1]->e_type << std::endl;
        unify(rhs_type, args[1]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "cannot implicitly assign an expression of type " << args[1]->e_type
           << " to a variable or pattern of type " << rhs_type << " in modifying method `" << sym::symbols.get_name(val)
           << "` : " << ue;
        lem.error(os.str());
      }
      e_type = rhs_type->args[1];
      TypeExpr::remove_indirect(e_type);
      // std::cerr << "result type is " << e_type << std::endl;
      return true;
    }
    case _CondExpr: {
      func_assert(args.size() == 3);
      auto flag_type = TypeExpr::new_atomic(_Int);
      try {
        unify(args[0]->e_type, flag_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "condition in a conditional expression has non-integer type " << args[0]->e_type << ": " << ue;
        lem.error(os.str());
      }
      try {
        unify(args[1]->e_type, args[2]->e_type);
      } catch (UnifyError& ue) {
        std::ostringstream os;
        os << "the two variants in a conditional expression have different types " << args[1]->e_type << " and "
           << args[2]->e_type << " : " << ue;
        lem.error(os.str());
      }
      e_type = args[1]->e_type;
      TypeExpr::remove_indirect(e_type);
      return true;
    }
  }
  return false;
}

int Expr::define_new_vars(CodeBlob& code) {
  switch (cls) {
    case _Tensor:
    case _MkTuple:
    case _TypeApply: {
      int res = 0;
      for (const auto& x : args) {
        res += x->define_new_vars(code);
      }
      return res;
    }
    case _Var:
      if (val < 0) {
        val = code.create_var(TmpVar::_Named, e_type, sym, &here);
        return 1;
      }
      break;
    case _Hole:
      if (val < 0) {
        val = code.create_var(TmpVar::_Tmp, e_type, nullptr, &here);
      }
      break;
  }
  return 0;
}

int Expr::predefine_vars() {
  switch (cls) {
    case _Tensor:
    case _MkTuple:
    case _TypeApply: {
      int res = 0;
      for (const auto& x : args) {
        res += x->predefine_vars();
      }
      return res;
    }
    case _Var:
      if (!sym) {
        func_assert(val < 0 && here.defined());
        if (prohibited_var_names.count(sym::symbols.get_name(~val))) {
          throw src::ParseError{
              here, PSTRING() << "symbol `" << sym::symbols.get_name(~val) << "` cannot be redefined as a variable"};
        }
        sym = sym::define_symbol(~val, false, here);
        // std::cerr << "predefining variable " << sym::symbols.get_name(~val) << std::endl;
        if (!sym) {
          throw src::ParseError{here, std::string{"redefined variable `"} + sym::symbols.get_name(~val) + "`"};
        }
        sym->value = new SymVal{SymVal::_Var, -1, e_type};
        return 1;
      }
      break;
  }
  return 0;
}

var_idx_t Expr::new_tmp(CodeBlob& code) const {
  return code.create_tmp_var(e_type, &here);
}

void add_set_globs(CodeBlob& code, std::vector<std::pair<SymDef*, var_idx_t>>& globs, const SrcLocation& here) {
  for (const auto& p : globs) {
    auto& op = code.emplace_back(here, Op::_SetGlob, std::vector<var_idx_t>{}, std::vector<var_idx_t>{ p.second }, p.first);
    op.set_impure(code);
  }
}

std::vector<var_idx_t> pre_compile_let(CodeBlob& code, Expr* lhs, Expr* rhs, const SrcLocation& here) {
  while (lhs->is_type_apply()) {
    lhs = lhs->args.at(0);
  }
  while (rhs->is_type_apply()) {
    rhs = rhs->args.at(0);
  }
  if (lhs->is_mktuple()) {
    if (rhs->is_mktuple()) {
      return pre_compile_let(code, lhs->args.at(0), rhs->args.at(0), here);
    }
    auto right = rhs->pre_compile(code);
    TypeExpr::remove_indirect(rhs->e_type);
    auto unpacked_type = rhs->e_type->args.at(0);
    std::vector<var_idx_t> tmp{code.create_tmp_var(unpacked_type, &rhs->here)};
    code.emplace_back(lhs->here, Op::_UnTuple, tmp, std::move(right));
    auto tvar = new Expr{Expr::_Var};
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
      if (!lval_globs && (var.cls & TmpVar::_Named)) {
        var.on_modification.push_back([&modified_vars, i, j, cur_ops = code.cur_ops, done = false](const SrcLocation &here) mutable {
          if (!done) {
            done = true;
            modified_vars.push_back({i, j, cur_ops});
          }
        });
      } else {
        var.on_modification.push_back([](const SrcLocation &) {
        });
      }
    }
  }
  for (const auto& list : res_lists) {
    for (var_idx_t v : list) {
      func_assert(!code.vars.at(v).on_modification.empty());
      code.vars.at(v).on_modification.pop_back();
    }
  }
  for (size_t idx = modified_vars.size(); idx--; ) {
    const ModifiedVar &m = modified_vars[idx];
    var_idx_t orig_v = res_lists[m.i][m.j];
    var_idx_t tmp_v = code.create_tmp_var(code.vars[orig_v].v_type, code.vars[orig_v].where.get());
    std::unique_ptr<Op> op = std::make_unique<Op>(*code.vars[orig_v].where, Op::_Let);
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
  if (lval_globs && !(cls == _Tensor || cls == _Var || cls == _Hole || cls == _TypeApply || cls == _GlobVar)) {
    std::cerr << "lvalue expression constructor is " << cls << std::endl;
    throw src::Fatal{"cannot compile lvalue expression with unknown constructor"};
  }
  switch (cls) {
    case _Tensor: {
      return pre_compile_tensor(args, code, lval_globs);
    }
    case _Apply: {
      func_assert(sym);
      std::vector<var_idx_t> res;
      SymDef* applied_sym = sym;
      auto func = dynamic_cast<SymValFunc*>(applied_sym->value);
      // replace `beginCell()` with `begin_cell()`
      if (func && func->is_just_wrapper_for_another_f()) {
        // body is { Op::_Import; Op::_Call; Op::_Return; }
        const std::unique_ptr<Op>& op_call = dynamic_cast<SymValCodeFunc*>(func)->code->ops->next;
        applied_sym = op_call->fun_ref;
        // a function may call anotherF with shuffled arguments: f(x,y) { return anotherF(y,x) }
        // then op_call looks like (_1,_0), so use op_call->right for correct positions in Op::_Call below
        // it's correct, since every argument has width 1
        std::vector<var_idx_t> res_inner = pre_compile_tensor(args, code, lval_globs);
        res.reserve(res_inner.size());
        for (var_idx_t right_idx : op_call->right) {
          res.emplace_back(res_inner[right_idx]);
        }
      } else {
        res = pre_compile_tensor(args, code, lval_globs);
      }
      auto rvect = new_tmp_vect(code);
      auto& op = code.emplace_back(here, Op::_Call, rvect, res, applied_sym);
      if (flags & _IsImpure) {
        op.set_impure(code);
      }
      return rvect;
    }
    case _TypeApply:
      return args[0]->pre_compile(code, lval_globs);
    case _Var:
    case _Hole:
      if (val < 0) {
        throw src::ParseError{here, "unexpected variable definition"};
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
          throw src::Fatal{"stack tuple used as a function"};
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
          throw src::ParseError(here, "Saving " + sym->name() + " into a variable will most likely lead to invalid usage, since it changes the order of variables on the stack");
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
    case _LetFirst: {
      auto rvect = new_tmp_vect(code);
      auto right = args[1]->pre_compile(code);
      std::vector<std::pair<SymDef*, var_idx_t>> local_globs;
      if (!lval_globs) {
        lval_globs = &local_globs;
      }
      auto left = args[0]->pre_compile(code, lval_globs);
      left.push_back(rvect[0]);
      for (var_idx_t v : left) {
        code.on_var_modification(v, here);
      }
      code.emplace_back(here, Op::_Let, std::move(left), std::move(right));
      add_set_globs(code, local_globs, here);
      return rvect;
    }
    case _MkTuple: {
      auto left = new_tmp_vect(code);
      auto right = args[0]->pre_compile(code);
      code.emplace_back(here, Op::_Tuple, left, std::move(right));
      return left;
    }
    case _CondExpr: {
      auto cond = args[0]->pre_compile(code);
      func_assert(cond.size() == 1);
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
      throw src::Fatal{"cannot compile expression with unknown constructor"};
  }
}

}  // namespace funC
