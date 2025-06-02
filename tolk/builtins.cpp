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
#include "generics-helpers.h"
#include "ast.h"

namespace tolk {
using namespace std::literals::string_literals;

// given func_type = `(slice, int) -> slice` and func flags, create SymLocalVarOrParameter
// currently (see at the bottom) parameters of built-in functions are unnamed:
// built-in functions are created using a resulting type
static std::vector<LocalVarData> define_builtin_parameters(const std::vector<TypePtr>& params_types, int func_flags) {
  // `loadInt()`, `storeInt()`: they accept `self` and mutate it; no other options available in built-ins for now
  bool is_mutate_self = func_flags & FunctionData::flagHasMutateParams;
  std::vector<LocalVarData> parameters;
  parameters.reserve(params_types.size());

  for (int i = 0; i < static_cast<int>(params_types.size()); ++i) {
    LocalVarData p_sym("", {}, params_types[i], nullptr, (i == 0 && is_mutate_self) * LocalVarData::flagMutateParameter, i);
    parameters.push_back(std::move(p_sym));
  }

  return parameters;
}

static void define_builtin_func(const std::string& name, const std::vector<TypePtr>& params_types, TypePtr return_type, const GenericsDeclaration* genericTs, const simple_compile_func_t& func, int flags) {
  auto* f_sym = new FunctionData(name, {}, "", nullptr, return_type, define_builtin_parameters(params_types, flags), flags, genericTs, nullptr, new FunctionBodyBuiltin(func), nullptr);
  G.symtable.add_function(f_sym);
}

static void define_builtin_method(const std::string& name, TypePtr receiver_type, const std::vector<TypePtr>& params_types, TypePtr return_type, const GenericsDeclaration* genericTs, const simple_compile_func_t& func, int flags,
                                std::initializer_list<int> arg_order = {}, std::initializer_list<int> ret_order = {}) {
  std::string method_name = name.substr(name.find('.') + 1);
  auto* f_sym = new FunctionData(name, {}, std::move(method_name), receiver_type, return_type, define_builtin_parameters(params_types, flags), flags, genericTs, nullptr, new FunctionBodyBuiltin(func), nullptr);
  f_sym->arg_order = arg_order;
  f_sym->ret_order = ret_order;
  G.symtable.add_function(f_sym);
  G.all_methods.push_back(f_sym);
}

void FunctionBodyBuiltin::compile(AsmOpList& dest, std::vector<VarDescr>& out, std::vector<VarDescr>& in,
                                     SrcLocation loc) const {
  dest << simple_compile(out, in, loc);
}

void FunctionBodyAsm::compile(AsmOpList& dest, SrcLocation loc) const {
  for (const AsmOp& op : ops) {
    AsmOp copy = op;
    copy.loc = loc;
    dest << std::move(copy);
  }
}


/*
 * 
 *   DEFINE BUILT-IN FUNCTIONS
 * 
 */

int emulate_negate(int a) {
  int f = VarDescr::_Pos | VarDescr::_Neg;
  if ((a & f) && (~a & f)) {
    a ^= f;
  }
  return a;
}

int emulate_add(int a, int b) {
  if (b & VarDescr::_Zero) {
    return a;
  } else if (a & VarDescr::_Zero) {
    return b;
  }
  int u = a & b, v = a | b;
  int r = VarDescr::_Int;
  int t = u & (VarDescr::_Pos | VarDescr::_Neg);
  if (v & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  // non-quiet addition always returns finite results!
  r |= t | VarDescr::_Finite;
  if (t) {
    r |= v & VarDescr::_NonZero;
  }
  r |= v & VarDescr::_Nan;
  if (u & (VarDescr::_Odd | VarDescr::_Even)) {
    r |= VarDescr::_Even;
  } else if (!(~v & (VarDescr::_Odd | VarDescr::_Even))) {
    r |= VarDescr::_Odd | VarDescr::_NonZero;
  }
  return r;
}

int emulate_sub(int a, int b) {
  return emulate_add(a, emulate_negate(b));
}

int emulate_mul(int a, int b) {
  if ((b & VarDescr::ConstOne) == VarDescr::ConstOne) {
    return a;
  } else if ((a & VarDescr::ConstOne) == VarDescr::ConstOne) {
    return b;
  }
  int u = a & b, v = a | b;
  int r = VarDescr::_Int;
  if (v & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  // non-quiet multiplication always yields finite results, if any
  r |= VarDescr::_Finite;
  if (v & VarDescr::_Zero) {
    // non-quiet multiplication
    // the result is zero, if any result at all
    return VarDescr::ConstZero;
  }
  if (u & (VarDescr::_Pos | VarDescr::_Neg)) {
    r |= VarDescr::_Pos;
  } else if (!(~v & (VarDescr::_Pos | VarDescr::_Neg))) {
    r |= VarDescr::_Neg;
  }
  r |= v & VarDescr::_Even;
  r |= u & (VarDescr::_Odd | VarDescr::_NonZero);
  return r;
}

int emulate_bitwise_and(int a, int b) {
  int both = a & b, any = a | b;
  int r = VarDescr::_Int;
  if (any & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  r |= VarDescr::_Finite;
  if (any & VarDescr::_Zero) {
    return VarDescr::ConstZero;
  }
  r |= both & (VarDescr::_Even | VarDescr::_Odd);
  if (both & VarDescr::_Odd) {
    r |= VarDescr::_NonZero;
  }
  return r;
}

int emulate_bitwise_or(int a, int b) {
  if (b & VarDescr::_Zero) {
    return a;
  } else if (a & VarDescr::_Zero) {
    return b;
  }
  int both = a & b, any = a | b;
  int r = VarDescr::_Int;
  if (any & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  r |= VarDescr::_Finite;
  r |= any & VarDescr::_NonZero;
  r |= any & VarDescr::_Odd;
  r |= both & VarDescr::_Even;
  return r;
}

int emulate_bitwise_xor(int a, int b) {
  if (b & VarDescr::_Zero) {
    return a;
  } else if (a & VarDescr::_Zero) {
    return b;
  }
  int both = a & b, any = a | b;
  int r = VarDescr::_Int;
  if (any & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  r |= VarDescr::_Finite;
  r |= both & VarDescr::_Even;
  if (both & VarDescr::_Odd) {
    r |= VarDescr::_Even;
  }
  return r;
}

int emulate_bitwise_not(int a) {
  if ((a & VarDescr::ConstZero) == VarDescr::ConstZero) {
    return VarDescr::ConstTrue;
  }
  if ((a & VarDescr::ConstTrue) == VarDescr::ConstTrue) {
    return VarDescr::ConstZero;
  }
  int a2 = a;
  int f = VarDescr::_Even | VarDescr::_Odd;
  if ((a2 & f) && (~a2 & f)) {
    a2 ^= f;
  }
  a2 &= ~(VarDescr::_Zero | VarDescr::_NonZero | VarDescr::_Pos | VarDescr::_Neg);
  if ((a & VarDescr::_Neg) && (a & VarDescr::_NonZero)) {
    a2 |= VarDescr::_Pos;
  }
  if (a & VarDescr::_Pos) {
    a2 |= VarDescr::_Neg;
  }
  return a2;
}

int emulate_lshift(int a, int b) {
  if (((a | b) & VarDescr::_Nan) || !(~b & (VarDescr::_Neg | VarDescr::_NonZero))) {
    return VarDescr::_Int | VarDescr::_Nan;
  }
  if (b & VarDescr::_Zero) {
    return a;
  }
  int t = ((b & VarDescr::_NonZero) ? VarDescr::_Even : 0);
  t |= b & VarDescr::_Finite;
  return emulate_mul(a, VarDescr::_Int | VarDescr::_Pos | VarDescr::_NonZero | t);
}

int emulate_div(int a, int b) {
  if ((b & VarDescr::ConstOne) == VarDescr::ConstOne) {
    return a;
  } else if ((b & VarDescr::ConstOne) == VarDescr::ConstOne) {
    return emulate_negate(a);
  }
  if (b & VarDescr::_Zero) {
    return VarDescr::_Int | VarDescr::_Nan;
  }
  int u = a & b, v = a | b;
  int r = VarDescr::_Int;
  if (v & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  // non-quiet division always yields finite results, if any
  r |= VarDescr::_Finite;
  if (a & VarDescr::_Zero) {
    // non-quiet division
    // the result is zero, if any result at all
    return VarDescr::ConstZero;
  }
  if (u & (VarDescr::_Pos | VarDescr::_Neg)) {
    r |= VarDescr::_Pos;
  } else if (!(~v & (VarDescr::_Pos | VarDescr::_Neg))) {
    r |= VarDescr::_Neg;
  }
  return r;
}

int emulate_rshift(int a, int b) {
  if (((a | b) & VarDescr::_Nan) || !(~b & (VarDescr::_Neg | VarDescr::_NonZero))) {
    return VarDescr::_Int | VarDescr::_Nan;
  }
  if (b & VarDescr::_Zero) {
    return a;
  }
  int t = ((b & VarDescr::_NonZero) ? VarDescr::_Even : 0);
  t |= b & VarDescr::_Finite;
  return emulate_div(a, VarDescr::_Int | VarDescr::_Pos | VarDescr::_NonZero | t);
}

int emulate_mod(int a, int b, int round_mode = -1) {
  if ((b & VarDescr::ConstOne) == VarDescr::ConstOne) {
    return VarDescr::ConstZero;
  }
  if (b & VarDescr::_Zero) {
    return VarDescr::_Int | VarDescr::_Nan;
  }
  int r = VarDescr::_Int;
  if ((a | b) & VarDescr::_Nan) {
    return r | VarDescr::_Nan;
  }
  // non-quiet division always yields finite results, if any
  r |= VarDescr::_Finite;
  if (a & VarDescr::_Zero) {
    // non-quiet division
    // the result is zero, if any result at all
    return VarDescr::ConstZero;
  }
  if (round_mode < 0) {
    r |= b & (VarDescr::_Pos | VarDescr::_Neg);
  } else if (round_mode > 0) {
    r |= emulate_negate(b) & (VarDescr::_Pos | VarDescr::_Neg);
  }
  if (b & VarDescr::_Even) {
    r |= a & (VarDescr::_Even | VarDescr::_Odd);
  }
  return r;
}

bool VarDescr::always_less(const VarDescr& other) const {
  if (is_int_const() && other.is_int_const()) {
    return int_const < other.int_const;
  }
  return (always_nonpos() && other.always_pos()) || (always_neg() && other.always_nonneg());
}

bool VarDescr::always_leq(const VarDescr& other) const {
  if (is_int_const() && other.is_int_const()) {
    return int_const <= other.int_const;
  }
  return always_nonpos() && other.always_nonneg();
}

bool VarDescr::always_greater(const VarDescr& other) const {
  return other.always_less(*this);
}

bool VarDescr::always_geq(const VarDescr& other) const {
  return other.always_leq(*this);
}

bool VarDescr::always_equal(const VarDescr& other) const {
  return is_int_const() && other.is_int_const() && *int_const == *other.int_const;
}

bool VarDescr::always_neq(const VarDescr& other) const {
  if (is_int_const() && other.is_int_const()) {
    return *int_const != *other.int_const;
  }
  return always_greater(other) || always_less(other) || (always_even() && other.always_odd()) ||
         (always_odd() && other.always_even());
}

AsmOp exec_op(SrcLocation loc, std::string op) {
  return AsmOp::Custom(loc, op);
}

AsmOp exec_op(SrcLocation loc, std::string op, int args, int retv = 1) {
  return AsmOp::Custom(loc, op, args, retv);
}

AsmOp exec_arg_op(SrcLocation loc, std::string op, long long arg, int args, int retv) {
  std::ostringstream os;
  os << arg << ' ' << op;
  return AsmOp::Custom(loc, os.str(), args, retv);
}

AsmOp exec_arg_op(SrcLocation loc, std::string op, td::RefInt256 arg, int args, int retv) {
  std::ostringstream os;
  os << arg << ' ' << op;
  return AsmOp::Custom(loc, os.str(), args, retv);
}

AsmOp exec_arg2_op(SrcLocation loc, std::string op, long long imm1, long long imm2, int args, int retv) {
  std::ostringstream os;
  os << imm1 << ' ' << imm2 << ' ' << op;
  return AsmOp::Custom(loc, os.str(), args, retv);
}

AsmOp push_const(SrcLocation loc, td::RefInt256 x) {
  return AsmOp::IntConst(loc, std::move(x));
}

static AsmOp compile_add(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const + y.int_const);
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, "integer overflow");
    }
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_add(x.val, y.val);
  if (y.is_int_const() && y.int_const->signed_fits_bits(8)) {
    y.unused();
    if (y.always_zero()) {
      return AsmOp::Nop(loc);
    }
    if (*y.int_const == 1) {
      return exec_op(loc, "INC", 1);
    }
    if (*y.int_const == -1) {
      return exec_op(loc, "DEC", 1);
    }
    return exec_arg_op(loc, "ADDCONST", y.int_const, 1);
  }
  if (x.is_int_const() && x.int_const->signed_fits_bits(8)) {
    x.unused();
    if (x.always_zero()) {
      return AsmOp::Nop(loc);
    }
    if (*x.int_const == 1) {
      return exec_op(loc, "INC", 1);
    }
    if (*x.int_const == -1) {
      return exec_op(loc, "DEC", 1);
    }
    return exec_arg_op(loc, "ADDCONST", x.int_const, 1);
  }
  return exec_op(loc, "ADD", 2);
}

static AsmOp compile_sub(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const - y.int_const);
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, "integer overflow");
    }
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_sub(x.val, y.val);
  if (y.is_int_const() && (-y.int_const)->signed_fits_bits(8)) {
    y.unused();
    if (y.always_zero()) {
      return {};
    }
    if (*y.int_const == 1) {
      return exec_op(loc, "DEC", 1);
    }
    if (*y.int_const == -1) {
      return exec_op(loc, "INC", 1);
    }
    return exec_arg_op(loc, "ADDCONST", -y.int_const, 1);
  }
  if (x.always_zero()) {
    x.unused();
    return exec_op(loc, "NEGATE", 1);
  }
  return exec_op(loc, "SUB", 2);
}

static AsmOp compile_unary_minus(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(-x.int_const);
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, "integer overflow");
    }
    x.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_negate(x.val);
  return exec_op(loc, "NEGATE", 1);
}

static AsmOp compile_unary_plus(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(x.int_const);
    x.unused();
    return push_const(loc, r.int_const);
  }
  r.val = x.val;
  return AsmOp::Nop(loc);
}

static AsmOp compile_logical_not(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, bool for_int_arg) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(x.int_const == 0 ? -1 : 0);
    x.unused();
    return push_const(loc, r.int_const);
  }
  r.val = VarDescr::ValBool;
  // for integers, `!var` is `var != 0`
  // for booleans, `!var` can be shortened to `~var` (works the same for 0/-1 but consumes less)
  return for_int_arg ? exec_op(loc, "0 EQINT", 1) : exec_op(loc, "NOT", 1);
}

static AsmOp compile_bitwise_and(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const & y.int_const);
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_bitwise_and(x.val, y.val);
  return exec_op(loc, "AND", 2);
}

static AsmOp compile_bitwise_or(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const | y.int_const);
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_bitwise_or(x.val, y.val);
  return exec_op(loc, "OR", 2);
}

static AsmOp compile_bitwise_xor(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const ^ y.int_const);
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_bitwise_xor(x.val, y.val);
  return exec_op(loc, "XOR", 2);
}

static AsmOp compile_bitwise_not(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(~x.int_const);
    x.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_bitwise_not(x.val);
  return exec_op(loc, "NOT", 1);
}

static AsmOp compile_mul_internal(VarDescr& r, VarDescr& x, VarDescr& y, SrcLocation loc) {
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const * y.int_const);
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, "integer overflow");
    }
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_mul(x.val, y.val);
  if (y.is_int_const()) {
    int k = is_pos_pow2(y.int_const);
    if (y.int_const->signed_fits_bits(8) && k < 0) {
      y.unused();
      if (y.always_zero() && x.always_finite()) {
        // dubious optimization: NaN * 0 = ?
        r.set_const(y.int_const);
        x.unused();
        return push_const(loc, r.int_const);
      }
      if (*y.int_const == 1 && x.always_finite()) {
        return AsmOp::Nop(loc);
      }
      if (*y.int_const == -1) {
        return exec_op(loc, "NEGATE", 1);
      }
      return exec_arg_op(loc, "MULCONST", y.int_const, 1);
    }
    if (k > 0) {
      y.unused();
      return exec_arg_op(loc, "LSHIFT#", k, 1);
    }
    if (k == 0) {
      y.unused();
      return AsmOp::Nop(loc);
    }
  }
  if (x.is_int_const()) {
    int k = is_pos_pow2(x.int_const);
    if (x.int_const->signed_fits_bits(8) && k < 0) {
      x.unused();
      if (x.always_zero() && y.always_finite()) {
        // dubious optimization: NaN * 0 = ?
        r.set_const(x.int_const);
        y.unused();
        return push_const(loc, r.int_const);
      }
      if (*x.int_const == 1 && y.always_finite()) {
        return AsmOp::Nop(loc);
      }
      if (*x.int_const == -1) {
        return exec_op(loc, "NEGATE", 1);
      }
      return exec_arg_op(loc, "MULCONST", x.int_const, 1);
    }
    if (k > 0) {
      x.unused();
      return exec_arg_op(loc, "LSHIFT#", k, 1);
    }
    if (k == 0) {
      x.unused();
      return AsmOp::Nop(loc);
    }
  }
  return exec_op(loc, "MUL", 2);
}

static AsmOp compile_mul(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  return compile_mul_internal(res[0], args[0], args[1], loc);
}

static AsmOp compile_lshift(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (y.is_int_const()) {
    auto yv = y.int_const->to_long();
    if (yv < 0 || yv > 256) {
      throw ParseError(loc, "lshift argument is out of range");
    } else if (x.is_int_const()) {
      r.set_const(x.int_const << (int)yv);
      if (!r.int_const->is_valid()) {
        throw ParseError(loc, "integer overflow");
      }
      x.unused();
      y.unused();
      return push_const(loc, r.int_const);
    }
  }
  r.val = emulate_lshift(x.val, y.val);
  if (y.is_int_const()) {
    int k = (int)(y.int_const->to_long());
    if (!k /* && x.always_finite() */) {
      // dubious optimization: what if x=NaN ?
      y.unused();
      return AsmOp::Nop(loc);
    }
    y.unused();
    return exec_arg_op(loc, "LSHIFT#", k, 1);
  }
  if (x.is_int_const()) {
    auto xv = x.int_const->to_long();
    if (xv == 1) {
      x.unused();
      return exec_op(loc, "POW2", 1);
    }
    if (xv == -1) {
      x.unused();
      return exec_op(loc, "-1 PUSHINT SWAP LSHIFT", 1);
    }
  }
  return exec_op(loc, "LSHIFT", 2);
}

static AsmOp compile_rshift(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc,
                     int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (y.is_int_const()) {
    auto yv = y.int_const->to_long();
    if (yv < 0 || yv > 256) {
      throw ParseError(loc, "rshift argument is out of range");
    } else if (x.is_int_const()) {
      r.set_const(td::rshift(x.int_const, (int)yv, round_mode));
      x.unused();
      y.unused();
      return push_const(loc, r.int_const);
    }
  }
  r.val = emulate_rshift(x.val, y.val);
  std::string rshift = (round_mode < 0 ? "RSHIFT" : (round_mode ? "RSHIFTC" : "RSHIFTR"));
  if (y.is_int_const()) {
    int k = (int)(y.int_const->to_long());
    if (!k /* && x.always_finite() */) {
      // dubious optimization: what if x=NaN ?
      y.unused();
      return AsmOp::Nop(loc);
    }
    y.unused();
    return exec_arg_op(loc, rshift + "#", k, 1);
  }
  return exec_op(loc, rshift, 2);
}

static AsmOp compile_div_internal(VarDescr& r, VarDescr& x, VarDescr& y, SrcLocation loc, int round_mode) {
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(div(x.int_const, y.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, *y.int_const == 0 ? "division by zero" : "integer overflow");
    }
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_div(x.val, y.val);
  if (y.is_int_const()) {
    if (*y.int_const == 0) {
      throw ParseError(loc, "division by zero");
    }
    if (*y.int_const == 1 && x.always_finite()) {
      y.unused();
      return AsmOp::Nop(loc);
    }
    if (*y.int_const == -1) {
      y.unused();
      return exec_op(loc, "NEGATE", 1);
    }
    int k = is_pos_pow2(y.int_const);
    if (k > 0) {
      y.unused();
      std::string op = "RSHIFT";
      if (round_mode >= 0) {
        op += (round_mode > 0 ? 'C' : 'R');
      }
      return exec_arg_op(loc, op + '#', k, 1);
    }
  }
  std::string op = "DIV";
  if (round_mode >= 0) {
    op += (round_mode > 0 ? 'C' : 'R');
  }
  return exec_op(loc, op, 2);
}

static AsmOp compile_div(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  return compile_div_internal(res[0], args[0], args[1], loc, round_mode);
}

static AsmOp compile_mod(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc,
                  int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(mod(x.int_const, y.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, *y.int_const == 0 ? "division by zero" : "integer overflow");
    }
    x.unused();
    y.unused();
    return push_const(loc, r.int_const);
  }
  r.val = emulate_mod(x.val, y.val);
  if (y.is_int_const()) {
    if (*y.int_const == 0) {
      throw ParseError(loc, "division by zero");
    }
    if ((*y.int_const == 1 || *y.int_const == -1) && x.always_finite()) {
      x.unused();
      y.unused();
      r.set_const(td::zero_refint());
      return push_const(loc, r.int_const);
    }
    int k = is_pos_pow2(y.int_const);
    if (k > 0) {
      y.unused();
      std::string op = "MODPOW2";
      if (round_mode >= 0) {
        op += (round_mode > 0 ? 'C' : 'R');
      }
      return exec_arg_op(loc, op + '#', k, 1);
    }
  }
  std::string op = "MOD";
  if (round_mode >= 0) {
    op += (round_mode > 0 ? 'C' : 'R');
  }
  return exec_op(loc, op, 2);
}

static AsmOp compile_muldiv(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc,
                     int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 3);
  VarDescr &r = res[0], &x = args[0], &y = args[1], &z = args[2];
  if (x.is_int_const() && y.is_int_const() && z.is_int_const()) {
    r.set_const(muldiv(x.int_const, y.int_const, z.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      throw ParseError(loc, *z.int_const == 0 ? "division by zero" : "integer overflow");
    }
    x.unused();
    y.unused();
    z.unused();
    return push_const(loc, r.int_const);
  }
  if (x.always_zero() || y.always_zero()) {
    // dubious optimization for z=0...
    x.unused();
    y.unused();
    z.unused();
    r.set_const(td::make_refint(0));
    return push_const(loc, r.int_const);
  }
  char c = (round_mode < 0) ? 0 : (round_mode > 0 ? 'C' : 'R');
  r.val = emulate_div(emulate_mul(x.val, y.val), z.val);
  if (z.is_int_const()) {
    if (*z.int_const == 0) {
      throw ParseError(loc, "division by zero");
    }
    if (*z.int_const == 1) {
      z.unused();
      return compile_mul_internal(r, x, y, loc);
    }
  }
  if (y.is_int_const() && *y.int_const == 1) {
    y.unused();
    return compile_div_internal(r, x, z, loc, round_mode);
  }
  if (x.is_int_const() && *x.int_const == 1) {
    x.unused();
    return compile_div_internal(r, y, z, loc, round_mode);
  }
  if (z.is_int_const()) {
    int k = is_pos_pow2(z.int_const);
    if (k > 0) {
      z.unused();
      std::string op = "MULRSHIFT";
      if (c) {
        op += c;
      }
      return exec_arg_op(loc, op + '#', k, 2);
    }
  }
  if (y.is_int_const()) {
    int k = is_pos_pow2(y.int_const);
    if (k > 0) {
      y.unused();
      std::string op = "LSHIFT#DIV";
      if (c) {
        op += c;
      }
      return exec_arg_op(loc, op, k, 2);
    }
  }
  if (x.is_int_const()) {
    int k = is_pos_pow2(x.int_const);
    if (k > 0) {
      x.unused();
      std::string op = "LSHIFT#DIV";
      if (c) {
        op += c;
      }
      return exec_arg_op(loc, op, k, 2);
    }
  }
  std::string op = "MULDIV";
  if (c) {
    op += c;
  }
  return exec_op(loc, op, 3);
}

// fun mulDivMod(x: int, y: int, z: int): (int, int)    asm "MULDIVMOD";
static AsmOp compile_muldivmod(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  return AsmOp::Custom(loc, "MULDIVMOD", 3, 2);
}

static int compute_compare(td::RefInt256 x, td::RefInt256 y, int mode) {
  int s = td::cmp(x, y);
  if (mode == 7) {
    return s;
  } else {
    return -((mode >> (1 - s)) & 1);
  }
}

// return value:
// 4 -> constant 1
// 2 -> constant 0
// 1 -> constant -1
// 3 -> 0 or -1
static int compute_compare(const VarDescr& x, const VarDescr& y, int mode) {
  switch (mode) {
    case 1:  // >
      return x.always_greater(y) ? 1 : (x.always_leq(y) ? 2 : 3);
    case 2:  // =
      return x.always_equal(y) ? 1 : (x.always_neq(y) ? 2 : 3);
    case 3:  // >=
      return x.always_geq(y) ? 1 : (x.always_less(y) ? 2 : 3);
    case 4:  // <
      return x.always_less(y) ? 1 : (x.always_geq(y) ? 2 : 3);
    case 5:  // <>
      return x.always_neq(y) ? 1 : (x.always_equal(y) ? 2 : 3);
    case 6:  // <=
      return x.always_leq(y) ? 1 : (x.always_greater(y) ? 2 : 3);
    case 7:  // <=>
      return x.always_less(y)
                 ? 1
                 : (x.always_equal(y)
                        ? 2
                        : (x.always_greater(y)
                               ? 4
                               : (x.always_leq(y) ? 3 : (x.always_geq(y) ? 6 : (x.always_neq(y) ? 5 : 7)))));
    default:
      return 7;
  }
}

static AsmOp compile_cmp_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, int mode) {
  tolk_assert(mode >= 1 && mode <= 7);
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    int v = compute_compare(x.int_const, y.int_const, mode);
    r.set_const(v);
    x.unused();
    y.unused();
    return mode == 7 ? push_const(loc, r.int_const) : AsmOp::BoolConst(loc, v != 0);
  }
  int v = compute_compare(x, y, mode);
  // std::cerr << "compute_compare(" << x << ", " << y << ", " << mode << ") = " << v << std::endl;
  tolk_assert(v);
  if (!(v & (v - 1))) {
    r.set_const(v - (v >> 2) - 2);
    x.unused();
    y.unused();
    return mode == 7 ? push_const(loc, r.int_const) : AsmOp::BoolConst(loc, v & 1);
  }
  r.val = ~0;
  if (v & 1) {
    r.val &= VarDescr::ConstTrue;
  }
  if (v & 2) {
    r.val &= VarDescr::ConstZero;
  }
  if (v & 4) {
    r.val &= VarDescr::ConstOne;
  }
  // std::cerr << "result: " << r << std::endl;
  static const char* cmp_int_names[] = {"", "GTINT", "EQINT", "GTINT", "LESSINT", "NEQINT", "LESSINT"};
  static const char* cmp_names[] = {"", "GREATER", "EQUAL", "GEQ", "LESS", "NEQ", "LEQ", "CMP"};
  static int cmp_int_delta[] = {0, 0, 0, -1, 0, 0, 1};
  if (mode != 7) {
    if (y.is_int_const() && y.int_const >= -128 && y.int_const <= 127) {
      y.unused();
      return exec_arg_op(loc, cmp_int_names[mode], y.int_const + cmp_int_delta[mode], 1);
    }
    if (x.is_int_const() && x.int_const >= -128 && x.int_const <= 127) {
      x.unused();
      mode = ((mode & 4) >> 2) | (mode & 2) | ((mode & 1) << 2);
      return exec_arg_op(loc, cmp_int_names[mode], x.int_const + cmp_int_delta[mode], 1);
    }
  }
  return exec_op(loc, cmp_names[mode], 2);
}

static AsmOp compile_throw(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.empty() && args.size() == 1);
  VarDescr& x = args[0];
  if (x.is_int_const() && x.int_const >= 0) {
    // in Fift assembler, "N THROW" is valid if N < 2048; for big N (particularly, widely used 0xFFFF)
    // we now still generate "N THROW", and later, in optimizer, transform it to "PUSHINT" + "THROWANY"
    x.unused();
    return exec_arg_op(loc, "THROW", x.int_const, 0, 0);
  } else {
    return exec_op(loc, "THROWANY", 1, 0);
  }
}

static AsmOp compile_throw_if_unless(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.empty() && args.size() == 3);
  VarDescr &x = args[0], &y = args[1], &z = args[2];
  if (!z.always_true() && !z.always_false()) {
    throw Fatal("invalid usage of built-in symbol");
  }
  bool mode = z.always_true();
  z.unused();
  std::string suff = (mode ? "IF" : "IFNOT");
  bool skip_cond = false;
  if (y.always_true() || y.always_false()) {
    y.unused();
    skip_cond = true;
    if (y.always_true() != mode) {
      x.unused();
      return AsmOp::Nop(loc);
    }
  }
  if (x.is_int_const() && x.int_const->unsigned_fits_bits(11)) {
    x.unused();
    return skip_cond ? exec_arg_op(loc, "THROW", x.int_const, 0, 0) : exec_arg_op(loc, "THROW"s + suff, x.int_const, 1, 0);
  } else {
    return skip_cond ? exec_op(loc, "THROWANY", 1, 0) : exec_op(loc, "THROWANY"s + suff, 2, 0);
  }
}

static AsmOp compile_throw_arg(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.empty() && args.size() == 2);
  VarDescr &x = args[1];
  if (x.is_int_const() && x.int_const->unsigned_fits_bits(11)) {
    x.unused();
    return exec_arg_op(loc, "THROWARG", x.int_const, 1, 0);
  } else {
    return exec_op(loc, "THROWARGANY", 2, 0);
  }
}

static AsmOp compile_bool_const(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, bool val) {
  tolk_assert(res.size() == 1 && args.empty());
  VarDescr& r = res[0];
  r.set_const(val ? -1 : 0);
  return AsmOp::Const(loc, val ? "TRUE" : "FALSE");
}

// fun slice.loadInt    (mutate self, len: int): int   asm(s len -> 1 0) "LDIX";
// fun slice.loadUint   (mutate self, len: int): int   asm( -> 1 0) "LDUX";
// fun slice.preloadInt (self, len: int): int          asm "PLDIX";
// fun slice.preloadUint(self, len: int): int          asm "PLDUX";
static AsmOp compile_fetch_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, bool fetch, bool sgnd) {
  tolk_assert(args.size() == 2 && res.size() == 1 + (unsigned)fetch);
  auto &y = args[1], &r = res.back();
  r.val = (sgnd ? VarDescr::FiniteInt : VarDescr::FiniteUInt);
  int v = -1;
  if (y.is_int_const() && y.int_const >= 0 && y.int_const <= 256) {
    v = (int)y.int_const->to_long();
    if (!v) {
      r.val = VarDescr::ConstZero;
    }
    if (v == 1) {
      r.val = (sgnd ? VarDescr::ValBool : VarDescr::ValBit);
    }
    if (v > 0) {
      y.unused();
      return exec_arg_op(loc, (fetch ? "LD"s : "PLD"s) + (sgnd ? 'I' : 'U'), v, 1, 1 + (unsigned)fetch);
    }
  }
  return exec_op(loc, (fetch ? "LD"s : "PLD"s) + (sgnd ? "IX" : "UX"), 2, 1 + (unsigned)fetch);
}

// fun builder.storeInt  (mutate self, x: int, len: int): self   asm(x b len) "STIX";
// fun builder.storeUint (mutate self, x: int, len: int): self   asm(x b len) "STUX";
static AsmOp compile_store_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, bool sgnd) {
  tolk_assert(args.size() == 3 && res.size() == 1);
  auto& z = args[2];
  if (z.is_int_const() && z.int_const > 0 && z.int_const <= 256) {
    z.unused();
    return exec_arg_op(loc, sgnd? "STI" : "STU", z.int_const, 2, 1);
  }
  return exec_op(loc, sgnd ? "STIX" : "STUX", 3, 1);
}

// fun slice.loadBits   (mutate self, len: int): self    asm(s len -> 1 0) "LDSLICEX"
// fun slice.preloadBits(self, len: int): slice          asm(s len -> 1 0) "PLDSLICEX"
static AsmOp compile_fetch_slice(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc, bool fetch) {
  tolk_assert(args.size() == 2 && res.size() == 1 + (unsigned)fetch);
  auto& y = args[1];
  int v = -1;
  if (y.is_int_const() && y.int_const > 0 && y.int_const <= 256) {
    v = (int)y.int_const->to_long();
    if (v > 0) {
      y.unused();
      return exec_arg_op(loc, fetch ? "LDSLICE" : "PLDSLICE", v, 1, 1 + (unsigned)fetch);
    }
  }
  return exec_op(loc, fetch ? "LDSLICEX" : "PLDSLICEX", 2, 1 + (unsigned)fetch);
}

// fun slice.tryStripPrefix(mutate self, prefix: int, prefixLen: int): bool
// constructs "x{...} SDBEGINSQ" for constant arguments
AsmOp compile_slice_sdbeginsq(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(args.size() == 3 && res.size() == 2);
  auto& prefix = args[1];
  auto& prefix_len = args[2];
  if (prefix.is_int_const() && prefix.int_const >= 0 && prefix.int_const->signed_fits_bits(50) &&
      prefix_len.is_int_const() && prefix_len.int_const > 0 && prefix_len.int_const->signed_fits_bits(16)) {
    prefix.unused();
    prefix_len.unused();
    StructData::PackOpcode opcode(prefix.int_const->to_long(), static_cast<int>(prefix_len.int_const->to_long()));
    return AsmOp::Custom(loc, opcode.format_as_slice() + " SDBEGINSQ", 0, 1);
  }
  throw ParseError(loc, "slice.tryStripPrefix can be used only with constant arguments");
}

// fun tuple.get<X>(t: tuple, index: int): X   asm "INDEXVAR";
static AsmOp compile_tuple_get(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(args.size() == 2 && res.size() == 1);
  auto& y = args[1];
  if (y.is_int_const() && y.int_const >= 0 && y.int_const < 16) {
    y.unused();
    return exec_arg_op(loc, "INDEX", y.int_const, 1, 1);
  }
  return exec_op(loc, "INDEXVAR", 2, 1);
}

// fun tuple.set<X>(mutate self: tuple, value: X, index: int): void   asm "SETINDEXVAR";
static AsmOp compile_tuple_set_at(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(args.size() == 3 && res.size() == 1);
  auto& y = args[2];
  if (y.is_int_const() && y.int_const >= 0 && y.int_const < 16) {
    y.unused();
    return exec_arg_op(loc, "SETINDEX", y.int_const, 1, 1);
  }
  return exec_op(loc, "SETINDEXVAR", 2, 1);
}

// fun debug.dumpStack(): void   asm "DUMPSTK";
static AsmOp compile_dumpstk(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  return AsmOp::Custom(loc, "DUMPSTK", 0, 0);
}

// fun debug.printString<T>(x: T): void   asm "STRDUMP";
static AsmOp compile_strdump(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  return AsmOp::Custom(loc, "STRDUMP DROP", 1, 1);
}

// fun debug.print<T>(x: T): void;
static AsmOp compile_debug_print_to_string(std::vector<VarDescr>&, std::vector<VarDescr>& args, SrcLocation loc) {
  int n = static_cast<int>(args.size());
  if (n == 1) {   // most common case
    return AsmOp::Custom(loc, "s0 DUMP DROP", 1, 1);
  }
  if (n > 15) {
    throw ParseError(loc, "call overflow, exceeds 15 elements");
  }
  std::string cmd;
  for (int i = n - 1; i >= 0; --i) {
    cmd += "s" + std::to_string(i) + " DUMP ";
  }
  cmd += std::to_string(n);
  cmd += " BLKDROP";
  return AsmOp::Custom(loc, cmd, n, n);
}

// fun T.__toTuple(self): void;    (T can be any number of slots, it works for structs and tensors)
static AsmOp compile_any_object_to_tuple(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1);
  int n = static_cast<int>(args.size());
  if (n > 15) {
    throw ParseError(loc, "call overflow, exceeds 15 elements");
  }
  return exec_op(loc, std::to_string(args.size()) + " TUPLE", n, 1);
}

// fun sizeof<T>(anything: T): int;        // (returns the number of stack elements)
static AsmOp compile_any_object_sizeof(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(res.size() == 1);
  int n = static_cast<int>(args.size());
  res[0].set_const(n);
  for (int i = 0; i < n; ++i) {
    args[i].unused();
  }
  return AsmOp::IntConst(loc, td::make_refint(n));
}

// fun ton(amount: slice): coins; ton("0.05") replaced by 50000000 at compile-time
// same for stringCrc32(constString: slice) and others
AsmOp compile_time_only_function(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  // all ton() invocations are constants, replaced by integers; no dynamic values allowed, no work at runtime
  tolk_assert(false);
  return AsmOp::Nop(loc);
}

// `null` literal is under the hood transformed to PUSHNULL
static AsmOp compile_push_null(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  return AsmOp::Const(loc, "PUSHNULL");
}

// fun __isNull<X>(X arg): bool
static AsmOp compile_is_null(std::vector<VarDescr>& res, std::vector<VarDescr>& args, SrcLocation loc) {
  tolk_assert(args.size() == 1 && res.size() == 1);
  res[0].val = VarDescr::ValBool;
  return exec_op(loc, "ISNULL", 1, 1);
}

// fun __expect_type(<expression>, "<expected_type>"): void;
static AsmOp compile_expect_type(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation loc) {
  // handled by type checker, does nothing at runtime
  return AsmOp::Nop(loc);
}


void define_builtins() {
  using namespace std::placeholders;

  TypePtr Unit = TypeDataVoid::create();
  TypePtr Int = TypeDataInt::create();
  TypePtr Bool = TypeDataBool::create();
  TypePtr Slice = TypeDataSlice::create();
  TypePtr Builder = TypeDataBuilder::create();
  TypePtr Tuple = TypeDataTuple::create();
  TypePtr Never = TypeDataNever::create();

  TypePtr typeT = TypeDataGenericT::create("T");
  const GenericsDeclaration* declGenericT = new GenericsDeclaration(std::vector<GenericsDeclaration::ItemT>{{"T", nullptr}}, 0);
  const GenericsDeclaration* declReceiverT = new GenericsDeclaration(std::vector<GenericsDeclaration::ItemT>{{"T", nullptr}}, 1);

  std::vector ParamsInt1 = {Int};
  std::vector ParamsInt2 = {Int, Int};
  std::vector ParamsInt3 = {Int, Int, Int};
  std::vector ParamsSliceInt = {Slice, Int};

  // these types are defined in stdlib, currently unknown
  // see patch_builtins_after_stdlib_loaded() below
  TypePtr debug = TypeDataUnknown::create();
  TypePtr CellT = TypeDataUnknown::create();
  TypePtr PackOptions = TypeDataUnknown::create();
  TypePtr UnpackOptions = TypeDataUnknown::create();

  // builtin operators
  // they are internally stored as functions, because at IR level, there is no difference
  // between calling `userAdd(a,b)` and `_+_(a,b)`
  // since they are registered in a global symtable, technically, they can even be referenced from Tolk code,
  // though it's a "hidden feature" and won't work well for overloads (`==` for int and bool, for example)

  // unary operators
  define_builtin_func("-_", ParamsInt1, Int, nullptr,
                              compile_unary_minus,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("+_", ParamsInt1, Int, nullptr,
                              compile_unary_plus,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("!_", ParamsInt1, Bool, nullptr,
                              std::bind(compile_logical_not, _1, _2, _3, true),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("!b_", {Bool}, Bool, nullptr,   // "overloaded" separate version for bool
                              std::bind(compile_logical_not, _1, _2, _3, false),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("~_", ParamsInt1, Int, nullptr,
                              compile_bitwise_not,
                                FunctionData::flagMarkedAsPure);

  // binary operators
  define_builtin_func("_+_", ParamsInt2, Int, nullptr,
                              compile_add,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_-_", ParamsInt2, Int, nullptr,
                              compile_sub,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_*_", ParamsInt2, Int, nullptr,
                              compile_mul,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, -1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_~/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, 0),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_^/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, 1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_%_", ParamsInt2, Int, nullptr,
                              std::bind(compile_mod, _1, _2, _3, -1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_<<_", ParamsInt2, Int, nullptr,
                              compile_lshift,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, -1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_~>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, 0),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_^>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, 1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_&_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_and,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_|_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_or,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_^_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_xor,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_==_", ParamsInt2, Int, nullptr,       // also works for bool
                              std::bind(compile_cmp_int, _1, _2, _3, 2),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_!=_", ParamsInt2, Int, nullptr,       // also works for bool
                              std::bind(compile_cmp_int, _1, _2, _3, 5),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_<_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 4),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_<=_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 6),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_>=_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 3),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("_<=>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 7),
                                FunctionData::flagMarkedAsPure);

  // special function used for internal compilation of some lexical constructs
  // for example, `throw 123;` is actually calling `__throw(123)`
  define_builtin_func("__true", {}, Bool, nullptr, /* AsmOp::Const("TRUE") */
                              std::bind(compile_bool_const, _1, _2, _3, true),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("__false", {}, Bool, nullptr, /* AsmOp::Const("FALSE") */
                              std::bind(compile_bool_const, _1, _2, _3, false),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("__null", {}, typeT, declGenericT,
                              compile_push_null,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("__isNull", {typeT}, Bool, declGenericT,
                              compile_is_null,
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("__throw", ParamsInt1, Never, nullptr,
                              compile_throw,
                                0);
  define_builtin_func("__throw_arg", {typeT, Int}, Never, declGenericT,
                              compile_throw_arg,
                                0);
  define_builtin_func("__throw_if_unless", ParamsInt3, Unit, nullptr,
                              compile_throw_if_unless,
                                0);

  // compile-time only functions, evaluated essentially at compile-time, no runtime implementation
  // they are placed in stdlib and marked as `builtin`
  // note their parameter being `unknown`: in order to `ton(1)` pass type inferring but fire a more gentle error later
  define_builtin_func("ton", {TypeDataUnknown::create()}, TypeDataCoins::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringCrc32", {TypeDataUnknown::create()}, TypeDataInt::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringCrc16", {TypeDataUnknown::create()}, TypeDataInt::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringSha256", {TypeDataUnknown::create()}, TypeDataInt::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringSha256_32", {TypeDataUnknown::create()}, TypeDataInt::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringToBase256", {TypeDataUnknown::create()}, TypeDataInt::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("stringHexToSlice", {TypeDataUnknown::create()}, TypeDataSlice::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);
  define_builtin_func("address", {TypeDataUnknown::create()}, TypeDataAddress::create(), nullptr,
                              compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal);

  // functions from stdlib marked as `builtin`, implemented at compiler level for optimizations
  // (for example, `loadInt(1)` is `1 LDI`, but `loadInt(n)` for non-constant requires it be on a stack and `LDIX`)
  define_builtin_func("mulDivFloor", ParamsInt3, Int, nullptr,
                              std::bind(compile_muldiv, _1, _2, _3, -1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("mulDivRound", ParamsInt3, Int, nullptr,
                              std::bind(compile_muldiv, _1, _2, _3, 0),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("mulDivCeil", ParamsInt3, Int, nullptr,
                              std::bind(compile_muldiv, _1, _2, _3, 1),
                                FunctionData::flagMarkedAsPure);
  define_builtin_func("mulDivMod", ParamsInt3, TypeDataTensor::create({Int, Int}), nullptr,
                              compile_muldivmod,
                                FunctionData::flagMarkedAsPure);
  define_builtin_method("slice.loadInt", Slice, ParamsSliceInt, Int, nullptr,
                              std::bind(compile_fetch_int, _1, _2, _3, true, true),
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf,
                                {}, {1, 0});
  define_builtin_method("slice.loadUint", Slice, ParamsSliceInt, Int, nullptr,
                              std::bind(compile_fetch_int, _1, _2, _3, true, false),
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf,
                                {}, {1, 0});
  define_builtin_method("slice.loadBits", Slice, ParamsSliceInt, Slice, nullptr,
                              std::bind(compile_fetch_slice, _1, _2, _3, true),
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf,
                                {}, {1, 0});
  define_builtin_method("slice.preloadInt", Slice, ParamsSliceInt, Int, nullptr,
                              std::bind(compile_fetch_int, _1, _2, _3, false, true),
                                FunctionData::flagMarkedAsPure | FunctionData::flagAcceptsSelf);
  define_builtin_method("slice.preloadUint", Slice, ParamsSliceInt, Int, nullptr,
                              std::bind(compile_fetch_int, _1, _2, _3, false, false),
                                FunctionData::flagMarkedAsPure | FunctionData::flagAcceptsSelf);
  define_builtin_method("slice.preloadBits", Slice, ParamsSliceInt, Slice, nullptr,
                              std::bind(compile_fetch_slice, _1, _2, _3, false),
                                FunctionData::flagMarkedAsPure | FunctionData::flagAcceptsSelf);
  define_builtin_method("slice.tryStripPrefix", Slice, {Slice, Int, Int}, Bool, nullptr,
                              compile_slice_sdbeginsq,
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf);
  define_builtin_method("builder.storeInt", Builder, {Builder, Int, Int}, Unit, nullptr,
                              std::bind(compile_store_int, _1, _2, _3, true),
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf | FunctionData::flagReturnsSelf,
                                {1, 0, 2}, {});
  define_builtin_method("builder.storeUint", Builder, {Builder, Int, Int}, Unit, nullptr,
                              std::bind(compile_store_int, _1, _2, _3, false),
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf | FunctionData::flagReturnsSelf,
                                {1, 0, 2}, {});
  define_builtin_method("tuple.get", Tuple, {Tuple, Int}, typeT, declGenericT,
                              compile_tuple_get,
                                FunctionData::flagMarkedAsPure | FunctionData::flagAcceptsSelf);
  define_builtin_method("tuple.set", Tuple, {Tuple, typeT, Int}, Unit, declGenericT,
                              compile_tuple_set_at,
                                FunctionData::flagMarkedAsPure | FunctionData::flagHasMutateParams | FunctionData::flagAcceptsSelf);
  define_builtin_method("debug.print", debug, {typeT}, Unit, declGenericT,
                                compile_debug_print_to_string,
                                FunctionData::flagAllowAnyWidthT);
  define_builtin_method("debug.printString", debug, {typeT}, Unit, declGenericT,
                                compile_strdump,
                                0);
  define_builtin_method("debug.dumpStack", debug, {}, Unit, nullptr,
                                compile_dumpstk,
                                0);
  define_builtin_func("sizeof", {typeT}, TypeDataInt::create(), declGenericT,
                                compile_any_object_sizeof,
                                FunctionData::flagMarkedAsPure | FunctionData::flagAllowAnyWidthT);

  // serialization/deserialization methods to/from cells (or, more low-level, slices/builders)
  // they work with structs (or, more low-level, with arbitrary types)
  define_builtin_method("T.toCell", typeT, {typeT, PackOptions}, CellT, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAcceptsSelf | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("T.fromCell", typeT, {TypeDataCell::create(), UnpackOptions}, typeT, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("T.fromSlice", typeT, {Slice, UnpackOptions}, typeT, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("T.estimatePackSize", typeT, {}, TypeDataBrackets::create({TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create(), TypeDataInt::create()}), declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("T.getDeclaredPackPrefix", typeT, {}, Int, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("T.getDeclaredPackPrefixLen", typeT, {}, Int, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeVal | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("Cell<T>.load", CellT, {CellT, UnpackOptions}, typeT, declReceiverT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAcceptsSelf | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("slice.loadAny", Slice, {Slice, UnpackOptions}, typeT, declGenericT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAcceptsSelf | FunctionData::flagHasMutateParams | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("slice.skipAny", Slice, {Slice, UnpackOptions}, Slice, declGenericT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAcceptsSelf | FunctionData::flagReturnsSelf | FunctionData::flagHasMutateParams | FunctionData::flagAllowAnyWidthT);
  define_builtin_method("builder.storeAny", Builder, {Builder, typeT, PackOptions}, Builder, declGenericT,
                                compile_time_only_function,
                                FunctionData::flagMarkedAsPure | FunctionData::flagCompileTimeGen | FunctionData::flagAcceptsSelf | FunctionData::flagReturnsSelf | FunctionData::flagHasMutateParams | FunctionData::flagAllowAnyWidthT);

  // functions not presented in stdlib at all
  // used in tolk-tester to check/expose internal compiler state
  // each of them is handled in a special way, search by its name
  define_builtin_func("__expect_type", {TypeDataUnknown::create(), Slice}, Unit, nullptr,
                                compile_expect_type,
                                FunctionData::flagMarkedAsPure);
  define_builtin_method("T.__toTuple", typeT, {typeT}, TypeDataTuple::create(), declReceiverT,
                                compile_any_object_to_tuple,
                                FunctionData::flagMarkedAsPure | FunctionData::flagAcceptsSelf | FunctionData::flagAllowAnyWidthT);
}

// there are some built-in functions that operate on types declared in stdlib (like Cell<T>)
// that's why these symbols were undefined, and when builtins were registered, they were set to unknown
// after all files have been loaded, symbols have been registered, and aliases exist,
// we patch that earlier registered built-in functions providing types that now exist
void patch_builtins_after_stdlib_loaded() {
  TypePtr typeT = TypeDataGenericT::create("T");
  StructPtr struct_debug = lookup_global_symbol("debug")->try_as<StructPtr>();
  TypePtr debug = TypeDataStruct::create(struct_debug);

  lookup_function("debug.print")->mutate()->receiver_type = debug;
  lookup_function("debug.printString")->mutate()->receiver_type = debug;
  lookup_function("debug.dumpStack")->mutate()->receiver_type = debug;

  StructPtr struct_ref_CellT = lookup_global_symbol("Cell")->try_as<StructPtr>();
  StructPtr struct_ref_PackOptions = lookup_global_symbol("PackOptions")->try_as<StructPtr>();
  StructPtr struct_ref_UnpackOptions = lookup_global_symbol("UnpackOptions")->try_as<StructPtr>();
  TypePtr CellT = TypeDataGenericTypeWithTs::create(struct_ref_CellT, nullptr, {typeT});
  TypePtr PackOptions = TypeDataStruct::create(struct_ref_PackOptions);
  TypePtr UnpackOptions = TypeDataStruct::create(struct_ref_UnpackOptions);

  // in stdlib, there is a default parameter `options = {}`; since default parameters are evaluated with AST,
  // emulate its presence in built-in functions; it looks ugly, but currently I don't have a better solution
  auto v_empty_PackOptions = createV<ast_object_literal>({}, nullptr, createV<ast_object_body>({}, {}));
  v_empty_PackOptions->assign_struct_ref(struct_ref_PackOptions);
  v_empty_PackOptions->assign_inferred_type(PackOptions);
  auto v_empty_UnpackOptions = createV<ast_object_literal>({}, nullptr, createV<ast_object_body>({}, {}));
  v_empty_UnpackOptions->assign_struct_ref(struct_ref_UnpackOptions);
  v_empty_UnpackOptions->assign_inferred_type(UnpackOptions);

  lookup_function("T.toCell")->mutate()->declared_return_type = CellT;
  lookup_function("T.toCell")->mutate()->parameters[1].declared_type = PackOptions;
  lookup_function("T.toCell")->mutate()->parameters[1].default_value = v_empty_PackOptions;
  lookup_function("T.fromCell")->mutate()->parameters[1].declared_type = UnpackOptions;
  lookup_function("T.fromCell")->mutate()->parameters[1].default_value = v_empty_UnpackOptions;
  lookup_function("T.fromSlice")->mutate()->parameters[1].declared_type = UnpackOptions;
  lookup_function("T.fromSlice")->mutate()->parameters[1].default_value = v_empty_UnpackOptions;
  lookup_function("Cell<T>.load")->mutate()->parameters[0].declared_type = CellT;
  lookup_function("Cell<T>.load")->mutate()->parameters[1].declared_type = UnpackOptions;
  lookup_function("Cell<T>.load")->mutate()->parameters[1].default_value = v_empty_UnpackOptions;
  lookup_function("Cell<T>.load")->mutate()->receiver_type = CellT;
  lookup_function("slice.loadAny")->mutate()->parameters[1].declared_type = UnpackOptions;
  lookup_function("slice.loadAny")->mutate()->parameters[1].default_value = v_empty_UnpackOptions;
  lookup_function("slice.skipAny")->mutate()->parameters[1].declared_type = UnpackOptions;
  lookup_function("slice.skipAny")->mutate()->parameters[1].default_value = v_empty_UnpackOptions;
  lookup_function("builder.storeAny")->mutate()->parameters[2].declared_type = PackOptions;
  lookup_function("builder.storeAny")->mutate()->parameters[2].default_value = v_empty_PackOptions;
}

}  // namespace tolk
