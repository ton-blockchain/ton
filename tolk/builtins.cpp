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

namespace tolk {
using namespace std::literals::string_literals;

// The parsed stdlib declaration owns the signature, generics, parameter names, and source-level flags.
// The compiler only attaches an implementation and metadata that cannot be expressed in Tolk.
static void redefine_builtin_body_impl(const char* name, FunctionBody body, int compiler_flags) {
  const Symbol* sym = lookup_global_symbol(name);
  FunctionPtr fun_ref = sym ? sym->try_as<FunctionPtr>() : nullptr;
  tolk_assert(fun_ref && !fun_ref->is_code_function());
  fun_ref->mutate()->body = body;
  fun_ref->mutate()->flags |= compiler_flags;
}

static void redefine_builtin_body(const char* name,
                                  const std::function<FunctionBodyBuiltinAsmOp::CompileToAsmOpImpl>& func,
                                  int compiler_flags = 0) {
  redefine_builtin_body_impl(name, new FunctionBodyBuiltinAsmOp(func), compiler_flags);
}

static void redefine_builtin_body(const char* name,
                                  const std::function<FunctionBodyBuiltinGenerateOps::GenerateOpsImpl>& func,
                                  int compiler_flags = 0) {
  redefine_builtin_body_impl(name, new FunctionBodyBuiltinGenerateOps(func), compiler_flags);
}

static void define_internal_builtin_func(const std::string& name, const std::vector<TypePtr>& params_types,
                                         TypePtr return_type, const GenericsDeclaration* genericTs,
                                         const std::function<FunctionBodyBuiltinAsmOp::CompileToAsmOpImpl>& func,
                                         int flags = FunctionData::flagRemovableIfUnused) {
  std::vector<LocalVarData> parameters;
  parameters.reserve(params_types.size());
  for (int i = 0; i < static_cast<int>(params_types.size()); ++i) {
    parameters.emplace_back("", nullptr, params_types[i], nullptr, 0, i);
  }

  auto* body = new FunctionBodyBuiltinAsmOp(func);
  auto* f_sym = new FunctionData(name, {}, "", nullptr, return_type,
                                 std::move(parameters), flags,
                                 FunctionInlineMode::notAnnotated, genericTs, nullptr, {}, body, nullptr);
  G.symtable.add_function(f_sym);
  G.all_functions.push_back(f_sym);
}

void FunctionBodyBuiltinAsmOp::compile(AsmOpList& dest, std::vector<VarDescr>& out, std::vector<VarDescr>& in,
                                     AnyV origin) const {
  dest << simple_compile(out, in, origin);
}

void FunctionBodyAsm::compile(AsmOpList& dest, AnyV origin) const {
  for (const AsmOp& op : ops) {
    AsmOp copy = op;
    copy.origin = origin;
    dest << std::move(copy);
  }
}

static std::string op_postfix_N_untuple(std::string cmd, int n_slots) {
  if (n_slots != 1) {
    cmd += " ";
    cmd += std::to_string(n_slots);
    cmd += n_slots < 16 ? " UNTUPLE" : " PUSHINT UNTUPLEVAR";
  }
  return cmd;
}

static std::string op_prefix_N_tuple(std::string cmd, int n_slots) {
  if (n_slots != 1) {
    std::string prefix = std::to_string(n_slots) + (n_slots < 16 ? " TUPLE " : " PUSHINT TUPLEVAR ");
    cmd = prefix + cmd;
  }
  return cmd;
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
  f = VarDescr::_Bit | VarDescr::_Bool;
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
  if ((b & (VarDescr::_NonZero | VarDescr::_Bit)) == (VarDescr::_NonZero | VarDescr::_Bit)) {
    return a;
  } else if ((a & (VarDescr::_NonZero | VarDescr::_Bit)) == (VarDescr::_NonZero | VarDescr::_Bit)) {
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
  if (u & (VarDescr::_Bit | VarDescr::_Bool)) {
    r |= VarDescr::_Bit;
  } else if (!(~v & (VarDescr::_Bit | VarDescr::_Bool))) {
    r |= VarDescr::_Bool;
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
  r |= both & (VarDescr::_Bit | VarDescr::_Bool);
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
  a2 &= ~(VarDescr::_Zero | VarDescr::_NonZero | VarDescr::_Bit | VarDescr::_Pos | VarDescr::_Neg);
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
  if ((b & (VarDescr::_NonZero | VarDescr::_Bit)) == (VarDescr::_NonZero | VarDescr::_Bit)) {
    return a;
  } else if ((b & (VarDescr::_NonZero | VarDescr::_Bool)) == (VarDescr::_NonZero | VarDescr::_Bool)) {
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
  if (u & (VarDescr::_Bit | VarDescr::_Bool)) {
    r |= VarDescr::_Bit;
  } else if (!(~v & (VarDescr::_Bit | VarDescr::_Bool))) {
    r |= VarDescr::_Bool;
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
  if ((b & (VarDescr::_NonZero | VarDescr::_Bit)) == (VarDescr::_NonZero | VarDescr::_Bit)) {
    return VarDescr::ConstZero;
  } else if ((b & (VarDescr::_NonZero | VarDescr::_Bool)) == (VarDescr::_NonZero | VarDescr::_Bool)) {
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
  if (a & (VarDescr::_Bit | VarDescr::_Bool)) {
    if (r & VarDescr::_Pos) {
      r |= VarDescr::_Bit;
    }
    if (r & VarDescr::_Neg) {
      r |= VarDescr::_Bool;
    }
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

static AsmOp exec_op(AnyV origin, std::string op, int args, int retv = 1) {
  return AsmOp::Custom(origin, op, args, retv);
}

static AsmOp exec_arg_op(AnyV origin, std::string op, long long arg, int args, int retv = 1) {
  std::ostringstream os;
  os << arg << ' ' << op;
  return AsmOp::Custom(origin, os.str(), args, retv);
}

static AsmOp exec_arg_op(AnyV origin, std::string op, td::RefInt256 arg, int args, int retv = 1) {
  std::ostringstream os;
  os << arg << ' ' << op;
  return AsmOp::Custom(origin, os.str(), args, retv);
}

AsmOp push_const(AnyV origin, td::RefInt256 x) {
  return AsmOp::IntConst(origin, std::move(x));
}

static AsmOp compile_add(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const + y.int_const);
    if (!r.int_const->is_valid()) {
      err("integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_add(x.val, y.val);
  if (y.is_int_const() && y.int_const->signed_fits_bits(8)) {
    y.unused();
    if (y.always_zero()) {
      return AsmOp::Nop(origin);
    }
    if (*y.int_const == 1) {
      return exec_op(origin, "INC", 1);
    }
    if (*y.int_const == -1) {
      return exec_op(origin, "DEC", 1);
    }
    return exec_arg_op(origin, "ADDCONST", y.int_const, 1);
  }
  if (x.is_int_const() && x.int_const->signed_fits_bits(8)) {
    x.unused();
    if (x.always_zero()) {
      return AsmOp::Nop(origin);
    }
    if (*x.int_const == 1) {
      return exec_op(origin, "INC", 1);
    }
    if (*x.int_const == -1) {
      return exec_op(origin, "DEC", 1);
    }
    return exec_arg_op(origin, "ADDCONST", x.int_const, 1);
  }
  return exec_op(origin, "ADD", 2);
}

static AsmOp compile_sub(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const - y.int_const);
    if (!r.int_const->is_valid()) {
      err("integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_sub(x.val, y.val);
  if (y.is_int_const() && (-y.int_const)->signed_fits_bits(8)) {
    y.unused();
    if (y.always_zero()) {
      return {};
    }
    if (*y.int_const == 1) {
      return exec_op(origin, "DEC", 1);
    }
    if (*y.int_const == -1) {
      return exec_op(origin, "INC", 1);
    }
    return exec_arg_op(origin, "ADDCONST", -y.int_const, 1);
  }
  if (x.always_zero()) {
    x.unused();
    return exec_op(origin, "NEGATE", 1);
  }
  return exec_op(origin, "SUB", 2);
}

static AsmOp compile_unary_minus(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(-x.int_const);
    if (!r.int_const->is_valid()) {
      err("integer overflow").fire(origin);
    }
    x.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_negate(x.val);
  return exec_op(origin, "NEGATE", 1);
}

static AsmOp compile_unary_plus(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(x.int_const);
    x.unused();
    return push_const(origin, r.int_const);
  }
  r.val = x.val;
  return AsmOp::Nop(origin);
}

static AsmOp compile_logical_not(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool for_int_arg) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(x.int_const == 0 ? -1 : 0);
    x.unused();
    return push_const(origin, r.int_const);
  }
  r.val = VarDescr::ValBool;
  // for integers, `!var` is `var != 0`
  // for booleans, `!var` can be shortened to `~var` (`NOT` consumes less gas than `0 EQINT`)
  // but we do insert a fake instruction `BOOLNOT` instead of `NOT` for future peephole optimizations;
  // for instance, `BOOLNOT + N THROWIF` => `N THROWIFNOT`, but for `NOT` (generally) it's incorrect;
  // un-optimized `BOOLNOT` are later replaced with a regular `NOT`
  return for_int_arg ? exec_op(origin, "0 EQINT", 1) : exec_op(origin, "BOOLNOT", 1);
}

static AsmOp compile_bitwise_and(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const & y.int_const);
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_bitwise_and(x.val, y.val);
  return exec_op(origin, "AND", 2);
}

static AsmOp compile_bitwise_or(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const | y.int_const);
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_bitwise_or(x.val, y.val);
  return exec_op(origin, "OR", 2);
}

static AsmOp compile_bitwise_xor(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const ^ y.int_const);
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_bitwise_xor(x.val, y.val);
  return exec_op(origin, "XOR", 2);
}

static AsmOp compile_bitwise_not(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  VarDescr &r = res[0], &x = args[0];
  if (x.is_int_const()) {
    r.set_const(~x.int_const);
    x.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_bitwise_not(x.val);
  return exec_op(origin, "NOT", 1);
}

static AsmOp compile_mul_internal(VarDescr& r, VarDescr& x, VarDescr& y, AnyV origin) {
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(x.int_const * y.int_const);
    if (!r.int_const->is_valid()) {
      err("integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
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
        return push_const(origin, r.int_const);
      }
      if (*y.int_const == 1 && x.always_finite()) {
        return AsmOp::Nop(origin);
      }
      if (*y.int_const == -1) {
        return exec_op(origin, "NEGATE", 1);
      }
      return exec_arg_op(origin, "MULCONST", y.int_const, 1);
    }
    if (k > 0) {
      y.unused();
      return exec_arg_op(origin, "LSHIFT#", k, 1);
    }
    if (k == 0) {
      y.unused();
      return AsmOp::Nop(origin);
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
        return push_const(origin, r.int_const);
      }
      if (*x.int_const == 1 && y.always_finite()) {
        return AsmOp::Nop(origin);
      }
      if (*x.int_const == -1) {
        return exec_op(origin, "NEGATE", 1);
      }
      return exec_arg_op(origin, "MULCONST", x.int_const, 1);
    }
    if (k > 0) {
      x.unused();
      return exec_arg_op(origin, "LSHIFT#", k, 1);
    }
    if (k == 0) {
      x.unused();
      return AsmOp::Nop(origin);
    }
  }
  return exec_op(origin, "MUL", 2);
}

static AsmOp compile_mul(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  return compile_mul_internal(res[0], args[0], args[1], origin);
}

static AsmOp compile_lshift(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (y.is_int_const()) {
    auto yv = y.int_const->to_long();
    if (yv < 0 || yv > 256) {
      err("lshift argument is out of range").fire(origin);
    } else if (x.is_int_const()) {
      r.set_const(x.int_const << (int)yv);
      if (!r.int_const->is_valid()) {
        err("integer overflow").fire(origin);
      }
      x.unused();
      y.unused();
      return push_const(origin, r.int_const);
    }
  }
  r.val = emulate_lshift(x.val, y.val);
  if (y.is_int_const()) {
    int k = (int)(y.int_const->to_long());
    if (!k /* && x.always_finite() */) {
      // dubious optimization: what if x=NaN ?
      y.unused();
      return AsmOp::Nop(origin);
    }
    y.unused();
    return exec_arg_op(origin, "LSHIFT#", k, 1);
  }
  if (x.is_int_const()) {
    auto xv = x.int_const->to_long();
    if (xv == 1) {
      x.unused();
      return exec_op(origin, "POW2", 1);
    }
    if (xv == -1) {
      x.unused();
      return exec_op(origin, "-1 PUSHINT SWAP LSHIFT", 1);
    }
  }
  return exec_op(origin, "LSHIFT", 2);
}

static AsmOp compile_rshift(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin,
                     int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (y.is_int_const()) {
    auto yv = y.int_const->to_long();
    if (yv < 0 || yv > 256) {
      err("rshift argument is out of range").fire(origin);
    } else if (x.is_int_const()) {
      r.set_const(td::rshift(x.int_const, (int)yv, round_mode));
      x.unused();
      y.unused();
      return push_const(origin, r.int_const);
    }
  }
  r.val = emulate_rshift(x.val, y.val);
  std::string rshift = (round_mode < 0 ? "RSHIFT" : (round_mode ? "RSHIFTC" : "RSHIFTR"));
  if (y.is_int_const()) {
    int k = (int)(y.int_const->to_long());
    if (!k /* && x.always_finite() */) {
      // dubious optimization: what if x=NaN ?
      y.unused();
      return AsmOp::Nop(origin);
    }
    y.unused();
    return exec_arg_op(origin, rshift + "#", k, 1);
  }
  return exec_op(origin, rshift, 2);
}

static AsmOp compile_div_internal(VarDescr& r, VarDescr& x, VarDescr& y, AnyV origin, int round_mode) {
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(div(x.int_const, y.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      err(*y.int_const == 0 ? "division by zero" : "integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_div(x.val, y.val);
  if (y.is_int_const()) {
    if (*y.int_const == 0) {
      err("division by zero").fire(origin);
    }
    if (*y.int_const == 1 && x.always_finite()) {
      y.unused();
      return AsmOp::Nop(origin);
    }
    if (*y.int_const == -1) {
      y.unused();
      return exec_op(origin, "NEGATE", 1);
    }
    int k = is_pos_pow2(y.int_const);
    if (k > 0) {
      y.unused();
      std::string op = "RSHIFT";
      if (round_mode >= 0) {
        op += (round_mode > 0 ? 'C' : 'R');
      }
      return exec_arg_op(origin, op + '#', k, 1);
    }
  }
  std::string op = "DIV";
  if (round_mode >= 0) {
    op += (round_mode > 0 ? 'C' : 'R');
  }
  return exec_op(origin, op, 2);
}

static AsmOp compile_div(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  return compile_div_internal(res[0], args[0], args[1], origin, round_mode);
}

static AsmOp compile_mod(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin,
                  int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    r.set_const(mod(x.int_const, y.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      err(*y.int_const == 0 ? "division by zero" : "integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    return push_const(origin, r.int_const);
  }
  r.val = emulate_mod(x.val, y.val);
  if (y.is_int_const()) {
    if (*y.int_const == 0) {
      err("division by zero").fire(origin);
    }
    if ((*y.int_const == 1 || *y.int_const == -1) && x.always_finite()) {
      x.unused();
      y.unused();
      r.set_const(td::zero_refint());
      return push_const(origin, r.int_const);
    }
    int k = is_pos_pow2(y.int_const);
    if (k > 0) {
      y.unused();
      std::string op = "MODPOW2";
      if (round_mode >= 0) {
        op += (round_mode > 0 ? 'C' : 'R');
      }
      return exec_arg_op(origin, op + '#', k, 1);
    }
  }
  std::string op = "MOD";
  if (round_mode >= 0) {
    op += (round_mode > 0 ? 'C' : 'R');
  }
  return exec_op(origin, op, 2);
}

static AsmOp compile_muldiv(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin,
                     int round_mode) {
  tolk_assert(res.size() == 1 && args.size() == 3);
  VarDescr &r = res[0], &x = args[0], &y = args[1], &z = args[2];
  if (x.is_int_const() && y.is_int_const() && z.is_int_const()) {
    r.set_const(muldiv(x.int_const, y.int_const, z.int_const, round_mode));
    if (!r.int_const->is_valid()) {
      err(*z.int_const == 0 ? "division by zero" : "integer overflow").fire(origin);
    }
    x.unused();
    y.unused();
    z.unused();
    return push_const(origin, r.int_const);
  }
  // 0 * y / z = 0 only when z is known non-zero; otherwise preserve potential div-by-zero at runtime
  if ((x.always_zero() || y.always_zero()) && z.always_nonzero()) {
    x.unused();
    y.unused();
    z.unused();
    r.set_const(td::make_refint(0));
    return push_const(origin, r.int_const);
  }
  char c = (round_mode < 0) ? 0 : (round_mode > 0 ? 'C' : 'R');
  r.val = emulate_div(emulate_mul(x.val, y.val), z.val);
  if (z.is_int_const()) {
    if (*z.int_const == 0) {
      err("division by zero").fire(origin);
    }
    if (*z.int_const == 1) {
      z.unused();
      return compile_mul_internal(r, x, y, origin);
    }
  }
  if (y.is_int_const() && *y.int_const == 1) {
    y.unused();
    return compile_div_internal(r, x, z, origin, round_mode);
  }
  if (x.is_int_const() && *x.int_const == 1) {
    x.unused();
    return compile_div_internal(r, y, z, origin, round_mode);
  }
  if (z.is_int_const()) {
    int k = is_pos_pow2(z.int_const);
    if (k > 0) {
      z.unused();
      std::string op = "MULRSHIFT";
      if (c) {
        op += c;
      }
      return exec_arg_op(origin, op + '#', k, 2);
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
      return exec_arg_op(origin, op, k, 2);
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
      return exec_arg_op(origin, op, k, 2);
    }
  }
  std::string op = "MULDIV";
  if (c) {
    op += c;
  }
  return exec_op(origin, op, 3);
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

static AsmOp compile_cmp_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, int mode) {
  tolk_assert(mode >= 1 && mode <= 7);
  tolk_assert(res.size() == 1 && args.size() == 2);
  VarDescr &r = res[0], &x = args[0], &y = args[1];
  if (x.is_int_const() && y.is_int_const()) {
    int v = compute_compare(x.int_const, y.int_const, mode);
    r.set_const(v);
    x.unused();
    y.unused();
    return mode == 7 ? push_const(origin, r.int_const) : AsmOp::BoolConst(origin, v != 0);
  }
  int v = compute_compare(x, y, mode);
  // std::cerr << "compute_compare(" << x << ", " << y << ", " << mode << ") = " << v << std::endl;
  tolk_assert(v);
  if (!(v & (v - 1))) {
    r.set_const(v - (v >> 2) - 2);
    x.unused();
    y.unused();
    return mode == 7 ? push_const(origin, r.int_const) : AsmOp::BoolConst(origin, v & 1);
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
      return exec_arg_op(origin, cmp_int_names[mode], y.int_const + cmp_int_delta[mode], 1);
    }
    if (x.is_int_const() && x.int_const >= -128 && x.int_const <= 127) {
      x.unused();
      mode = ((mode & 4) >> 2) | (mode & 2) | ((mode & 1) << 2);
      return exec_arg_op(origin, cmp_int_names[mode], x.int_const + cmp_int_delta[mode], 1);
    }
  }
  return exec_op(origin, cmp_names[mode], 2);
}

static AsmOp compile_throw(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.empty() && args.size() == 1);
  VarDescr& x = args[0];
  if (x.is_int_const() && x.int_const >= 0 && x.int_const < 65536) {
    // in Fift assembler, "N THROW" is valid if N < 2048; for big N (particularly, widely used 0xFFFF)
    // we now still generate "N THROW", and later, in optimizer, transform it to "PUSHINT" + "THROWANY"
    x.unused();
    return exec_arg_op(origin, "THROW", x.int_const, 0, 0);
  } else {
    return exec_op(origin, "THROWANY", 1, 0);
  }
}

static AsmOp compile_throw_if_ifnot(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool is_ifnot) {
  tolk_assert(res.empty() && args.size() == 2);
  VarDescr &x = args[0], &y = args[1];

  bool skip_all = is_ifnot ? y.always_true() : y.always_false();    // __throw_if(ex, false): do nothing
  if (skip_all) {
    x.unused();
    y.unused();
    return AsmOp::Nop(origin);
  }

  bool skip_cond = y.always_true() || y.always_false();
  if (skip_cond) {
    y.unused();
  }

  if (x.is_int_const() && x.int_const->unsigned_fits_bits(11)) {
    x.unused();
    std::string cond_asm = is_ifnot ? "THROWIFNOT" : "THROWIF";
    return skip_cond ? exec_arg_op(origin, "THROW", x.int_const, 0, 0) : exec_arg_op(origin, cond_asm, x.int_const, 1, 0);
  } else {
    std::string cond_asm = is_ifnot ? "THROWANYIFNOT" : "THROWANYIF";
    return skip_cond ? exec_op(origin, "THROWANY", 1, 0) : exec_op(origin, cond_asm, 2, 0);
  }
}

static AsmOp compile_calc_InMessage_originalForwardFee(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  return exec_op(origin, "GETORIGINALFWDFEE", 2);
}

static AsmOp compile_calc_InMessage_getInMsgParam(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  // instead of "0 INMSGPARAM", generate "INMSG_BOUNCE", etc. — these are aliases in Asm.fif
  static const char* aliases[] = {
    "INMSG_BOUNCE", "INMSG_BOUNCED", "INMSG_SRC", "INMSG_FWDFEE", "INMSG_LT", "INMSG_UTIME", "INMSG_ORIGVALUE", "INMSG_VALUE", "INMSG_VALUEEXTRA", "INMSG_STATEINIT",
  };
  tolk_assert(res.size() == 1 && args.size() == 1 && args[0].is_int_const());
  args[0].unused();
  uint64_t idx = static_cast<uint64_t>(args[0].int_const->to_long());
  if (idx < std::size(aliases)) {
    return exec_op(origin, aliases[idx], 0);
  }
  return exec_arg_op(origin, "INMSGPARAM", args[0].int_const, 1);
}

static AsmOp compile_throw_arg(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.empty() && args.size() == 2);
  VarDescr &x = args[1];
  if (x.is_int_const() && x.int_const->unsigned_fits_bits(11)) {
    x.unused();
    return exec_arg_op(origin, "THROWARG", x.int_const, 1, 0);
  } else {
    return exec_op(origin, "THROWARGANY", 2, 0);
  }
}

// `x ? y : z` can be compiled as `CONDSEL` asm instruction if y and z are don't require evaluation
static AsmOp compile_ternary_as_condsel(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 3);
  VarDescr& cond = args[0];     // args = [ cond, when_true, when_false ]
  if (cond.always_true()) {
    cond.unused();
    args[2].unused();
    return AsmOp::Nop(origin);
  }
  if (cond.always_false()) {
    cond.unused();
    args[1].unused();
    return AsmOp::Nop(origin);
  }
  return exec_op(origin, "CONDSEL", 3, 1);
}

static AsmOp compile_bool_const(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool val) {
  tolk_assert(res.size() == 1 && args.empty());
  VarDescr& r = res[0];
  r.set_const(val ? -1 : 0);
  return AsmOp::Const(origin, val ? "TRUE" : "FALSE");
}

// fun slice.loadInt    (mutate self, len: int): int   asm(s len -> 1 0) "LDIX";
// fun slice.loadUint   (mutate self, len: int): int   asm( -> 1 0) "LDUX";
// fun slice.preloadInt (self, len: int): int          asm "PLDIX";
// fun slice.preloadUint(self, len: int): int          asm "PLDUX";
static AsmOp compile_fetch_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool fetch, bool sgnd) {
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
      return exec_arg_op(origin, (fetch ? "LD"s : "PLD"s) + (sgnd ? 'I' : 'U'), v, 1, 1 + (unsigned)fetch);
    }
  }
  return exec_op(origin, (fetch ? "LD"s : "PLD"s) + (sgnd ? "IX" : "UX"), 2, 1 + (unsigned)fetch);
}

// fun __loadVarInt(s: slice, bits: int, unsigned: bool): (int, slice)
static AsmOp compile_fetch_varint(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 3 && res.size() == 2);
  // it's a hidden function for auto-serialization (not exposed to stdlib), to bits/unsigned are not dynamic
  tolk_assert(args[1].is_int_const() && args[2].is_int_const());
  uint64_t n_bits = static_cast<uint64_t>(args[1].int_const->to_long());
  uint64_t is_unsigned = static_cast<uint64_t>(args[2].int_const->to_long());

  args[1].unused();
  args[2].unused();
  if (n_bits == 16) {
    return exec_op(origin, is_unsigned ? "LDVARUINT16" : "LDVARINT16", 1, 2);
  }
  if (n_bits == 32) {
    return exec_op(origin, is_unsigned ? "LDVARUINT32" : "LDVARINT32", 1, 2);
  }
  tolk_assert(false);
}

// fun builder.storeInt  (mutate self, x: int, len: int): self   asm(x b len) "STIX";
// fun builder.storeUint (mutate self, x: int, len: int): self   asm(x b len) "STUX";
static AsmOp compile_store_int(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool sgnd) {
  tolk_assert(args.size() == 3 && res.size() == 1);
  auto& x = args[1];
  auto& z = args[2];
  // purpose: to merge consecutive `b.storeUint(0, 1).storeUint(1, 1)` into one "1 PUSHINT + 2 STU",
  // when constant arguments are passed, keep them as a separate (fake) instruction, to be handled by optimizer later
  bool value_and_len_is_const = z.is_int_const() && x.is_int_const();
  if (value_and_len_is_const && x.int_const >= 0 && z.int_const > 0 && z.int_const <= 256) {
    // don't handle negative numbers or potential overflow, merging them is incorrect
    int len = static_cast<int>(z.int_const->to_long());
    if (x.int_const->fits_bits(len, sgnd)) {
      z.unused();
      x.unused();
      return AsmOp::Custom(origin, "MY_store_int"s + (sgnd ? "I " : "U ") + x.int_const->to_dec_string() + " " + z.int_const->to_dec_string(), 1);
    }
  }
  if (z.is_int_const() && z.int_const > 0 && z.int_const <= 256) {
    z.unused();
    return exec_arg_op(origin, sgnd? "STI" : "STU", z.int_const, 2, 1);
  }
  return exec_op(origin, sgnd ? "STIX" : "STUX", 3, 1);
}

// fun __storeVarInt(b: builder, x: int, bits: int, unsigned: bool): builder
static AsmOp compile_store_varint(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 4 && res.size() == 1);
  // it's a hidden function for auto-serialization (not exposed to stdlib), to bits/unsigned are not dynamic
  tolk_assert(args[2].is_int_const() && args[3].is_int_const());
  uint64_t n_bits = static_cast<uint64_t>(args[2].int_const->to_long());
  uint64_t is_unsigned = static_cast<uint64_t>(args[3].int_const->to_long());

  args[2].unused();
  args[3].unused();
  if (n_bits == 16) {
    return exec_op(origin, is_unsigned ? "STVARUINT16" : "STVARINT16", 2, 1);
  }
  if (n_bits == 32) {
    return exec_op(origin, is_unsigned ? "STVARUINT32" : "STVARINT32", 2, 1);
  }
  tolk_assert(false);
}

// fun builder.storeBool(mutate self, value: bool): self   asm( -> 1 0) "1 STI";
static AsmOp compile_store_bool(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 2 && res.size() == 1);
  auto& v = args[1];
  // same purpose as for storeInt/storeUint above
  // (particularly, `b.storeUint(const_int,32).storeBool(const_bool)` will be joined)
  if (v.is_int_const() && v.int_const == 0) {
    v.unused();
    return AsmOp::Custom(origin, "MY_store_intU 0 1", 1);
  }
  if (v.is_int_const() && v.int_const == -1) {
    v.unused();
    return AsmOp::Custom(origin, "MY_store_intU 1 1", 1);
  }
  return exec_op(origin, "1 STI", 2, 1);
}

// fun builder.storeCoins(mutate self, value: coins): self   asm "STGRAMS";
static AsmOp compile_store_coins(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 2 && res.size() == 1);
  auto& v = args[1];
  // same purpose as for storeInt/storeUint above
  // (particularly, `b.storeUint(const_int,32).storeCoins(const_zero)` will be joined)
  if (v.is_int_const() && v.int_const == 0) {
    v.unused();
    return AsmOp::Custom(origin, "MY_store_intU 0 4", 1);
  }
  return exec_op(origin, "STGRAMS", 2, 1);
}

// fun slice.loadBits   (mutate self, len: int): self    asm(s len -> 1 0) "LDSLICEX"
// fun slice.preloadBits(self, len: int): slice          asm(s len -> 1 0) "PLDSLICEX"
static AsmOp compile_fetch_slice(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin, bool fetch) {
  tolk_assert(args.size() == 2 && res.size() == 1 + (unsigned)fetch);
  auto& y = args[1];
  int v = -1;
  if (y.is_int_const() && y.int_const > 0 && y.int_const <= 256) {
    v = (int)y.int_const->to_long();
    if (v > 0) {
      y.unused();
      return exec_arg_op(origin, fetch ? "LDSLICE" : "PLDSLICE", v, 1, 1 + (unsigned)fetch);
    }
  }
  return exec_op(origin, fetch ? "LDSLICEX" : "PLDSLICEX", 2, 1 + (unsigned)fetch);
}

// fun __tryStripPrefix(s: slice, prefix: int, prefixLen: int): (slice, bool)
// constructs "x{...} SDBEGINSQ" for constant arguments
static AsmOp compile_slice_sdbeginsq(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 3 && res.size() == 2);
  auto& prefix = args[1];
  auto& prefix_len = args[2];
  if (prefix.is_int_const() && prefix.int_const >= 0 && prefix.int_const->signed_fits_bits(50) &&
      prefix_len.is_int_const() && prefix_len.int_const > 0 && prefix_len.int_const < 1024) {
    prefix.unused();
    prefix_len.unused();
    StructData::PackOpcode opcode(prefix.int_const->to_long(), static_cast<int>(prefix_len.int_const->to_long()));
    return AsmOp::Custom(origin, opcode.format_as_string(true) + " SDBEGINSQ", 0, 1);
  }
  err("__tryStripPrefix can be used only with constant arguments").fire(origin);
}

// fun slice.skipBits(mutate self, len: int): self    "SDSKIPFIRST"
static AsmOp compile_skip_bits_in_slice(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 2 && res.size() == 1);
  auto& len = args[1];
  // same technique as for storeUint:
  // consecutive `s.skipBits(8).skipBits(const_var_16)` will be joined into a single 24
  // to track this, represent it as a separate fake instruction to be detected by optimizer later
  if (len.is_int_const() && len.int_const >= 0 && len.int_const < 1024) {
    len.unused();
    return AsmOp::Custom(origin, "MY_skip_bits " + len.int_const->to_dec_string(), 1);
  }
  return exec_op(origin, "SDSKIPFIRST", 2, 1);
}


// fun array<T>.get(self, index: int): T
static AsmOp compile_array_get(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 2);
  int n_slots = static_cast<int>(res.size());
  auto& y = args[1];
  if (y.is_int_const() && y.int_const >= 0 && y.int_const < 16) {
    y.unused();
    return exec_arg_op(origin, op_postfix_N_untuple("INDEX", n_slots), y.int_const, 1, n_slots);
  }
  return exec_op(origin, op_postfix_N_untuple("INDEXVAR", n_slots), 2, n_slots);
}

// fun array<T>.set(mutate self, value: T, index: int): void
static AsmOp compile_array_set_at(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() >= 2 && res.size() == 1);
  int n_slots = static_cast<int>(args.size() - 2);
  auto& y = args.back();
  if (y.is_int_const() && y.int_const >= 0 && y.int_const < 16) {
    y.unused();
    return exec_op(origin, op_prefix_N_tuple(y.int_const->to_dec_string() + " SETINDEX", n_slots), n_slots + 1, 1);
  }
  if (n_slots == 1) {
    return exec_op(origin, "SETINDEXVAR", 3, 1);
  }
  if (n_slots < 1 || n_slots > 16) {
    err("array.set is supported for 1..16 slots ({} stack slots here)", n_slots).fire(origin);
  }
  std::string prefix_N_tuple_stack = std::to_string(n_slots) + " 1 BLKSWAP " + std::to_string(n_slots) + (n_slots > 15 ? " PUSHINT TUPLEVAR " : " TUPLE ") + "SWAP ";  
  return exec_op(origin, prefix_N_tuple_stack + "SETINDEXVAR", n_slots + 2, 1);
}

// fun array<T>.push(mutate self, value: T): void;
static AsmOp compile_array_push(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1);
  int n_slots = static_cast<int>(args.size() - 1);
  return exec_op(origin, op_prefix_N_tuple("TPUSH", n_slots), n_slots, 1);
}

// fun array<T>.size(self): int;
static AsmOp compile_array_size(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1 && args.size() == 1);
  return exec_op(origin, "TLEN", 1, 1);
}

// fun array<T>.last(self): T;
static AsmOp compile_array_last(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 1);
  int n_slots = static_cast<int>(res.size());
  return exec_op(origin, op_postfix_N_untuple("LAST", n_slots), 1, n_slots);
}

// fun array<T>.first(self): T;
static AsmOp compile_array_first(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 1);
  int n_slots = static_cast<int>(res.size());
  return exec_op(origin, op_postfix_N_untuple("FIRST", n_slots), 1, n_slots);
}

// fun array<T>.pop(mutate self): T;
static AsmOp compile_array_pop(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() >= 1 && args.size() == 1);
  int n_slots = static_cast<int>(res.size() - 1);
  return exec_op(origin, op_postfix_N_untuple("TPOP", n_slots), 1, n_slots + 1);
}

// fun debug.print<T>(x: T): void;
static AsmOp compile_debug_print_to_string(std::vector<VarDescr>&, std::vector<VarDescr>& args, AnyV origin) {
  int n = static_cast<int>(args.size());
  if (n == 1) {   // most common case
    return AsmOp::Custom(origin, "s0 DUMP DROP", 1, 1);
  }
  if (n > 15) {
    err("call overflow, exceeds 15 elements").fire(origin);
  }
  std::string cmd;
  for (int i = n - 1; i >= 0; --i) {
    cmd += "s" + std::to_string(i) + " DUMP ";
  }
  cmd += std::to_string(n);
  cmd += " BLKDROP";
  return AsmOp::Custom(origin, cmd, n, n);
}

// fun T.toTuple(self): array<unknown>;    (T can be any number of slots, it works for structs and tensors)
static AsmOp compile_T_to_tuple(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1);
  int n_slots = static_cast<int>(args.size());
  if (UNLIKELY(n_slots >= 255)) {
    err("tuple overflow").fire(origin);
  }
  std::string op_make_tuple = std::to_string(n_slots) + (n_slots > 15 ? " PUSHINT TUPLEVAR" : " TUPLE");  
  return exec_op(origin, op_make_tuple, n_slots, 1);
}

// fun T.fromTuple(t: array<unknown>): T;
static AsmOp compile_T_from_tuple(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 1);
  int n_slots = static_cast<int>(res.size());
  if (UNLIKELY(n_slots >= 255)) {
    err("tuple overflow").fire(origin);
  }
  std::string op_un_tuple = std::to_string(n_slots) + (n_slots > 15 ? " PUSHINT UNTUPLEVAR" : " UNTUPLE");  
  return exec_op(origin, op_un_tuple, 1, n_slots);
}

// fun reflect.stackSizeOfObject<T>(anything: T): int;
static AsmOp compile_reflect_stackSizeOfObject(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(res.size() == 1);
  int n = static_cast<int>(args.size());
  res[0].set_const(n);
  for (int i = 0; i < n; ++i) {
    args[i].unused();
  }
  return AsmOp::IntConst(origin, td::make_refint(n));
}

// fun reflect.stackSizeOf<T>(): int;
std::vector<var_idx_t> generate_reflect_stackSizeOf(FunctionPtr called_f, CodeBlob& code, AnyV origin, const std::vector<std::vector<var_idx_t>>& args) {
  TypePtr typeT = called_f->substitutedTs->typeT_at(0);
  return {code.create_int(origin, typeT->get_width_on_stack(), "(stack-w)")};
}


// fun grams(amount: slice): coins; grams("0.05") replaced by 50000000 at compile-time
// same for stringCrc32(constString: slice) and others
static AsmOp compile_time_only_function(std::vector<VarDescr>&, std::vector<VarDescr>&, AnyV origin) {
  // all grams() invocations are constants, replaced by integers; no dynamic values allowed, no work at runtime
  tolk_assert(false);
  return AsmOp::Nop(origin);
}

// `null` literal is under the hood transformed to PUSHNULL
static AsmOp compile_push_null(std::vector<VarDescr>&, std::vector<VarDescr>&, AnyV origin) {
  return AsmOp::Const(origin, "PUSHNULL");
}

// fun __isNull<X>(X arg): bool
static AsmOp compile_is_null(std::vector<VarDescr>& res, std::vector<VarDescr>& args, AnyV origin) {
  tolk_assert(args.size() == 1 && res.size() == 1);
  auto &x = args[0], &r = res[0];
  if (x.always_null() || x.always_not_null()) {
    x.unused();
    r.set_const(x.always_null() ? -1 : 0);
    return push_const(origin, r.int_const);
  }
  res[0].val = VarDescr::ValBool;
  return exec_op(origin, "ISNULL", 1, 1);
}

// fun __expect_type(<expression>, "<expected_type>"): void;
static AsmOp compile_expect_type(std::vector<VarDescr>&, std::vector<VarDescr>& args, AnyV origin) {
  for (VarDescr& a : args) {
    a.unused();
  }
  // handled by type checker, does nothing at runtime
  return AsmOp::Nop(origin);
}

// implemented in dedicated files

using GenerateOpsImpl = FunctionBodyBuiltinGenerateOps::GenerateOpsImpl;
using CompileToAsmOpImpl = FunctionBodyBuiltinAsmOp::CompileToAsmOpImpl;

GenerateOpsImpl generate_T_toCell;
GenerateOpsImpl generate_builder_storeAny;
GenerateOpsImpl generate_T_fromSlice;
GenerateOpsImpl generate_slice_loadAny;
GenerateOpsImpl generate_T_fromCell;
GenerateOpsImpl generate_T_forceLoadLazyObject;
GenerateOpsImpl generate_slice_skipAny;
GenerateOpsImpl generate_reflect_estimateSerializationOf;
GenerateOpsImpl generate_reflect_serializationPrefixOf;

GenerateOpsImpl generate_createMessage;
GenerateOpsImpl generate_createExternalLogMessage;
GenerateOpsImpl generate_address_buildInAnotherShard;
GenerateOpsImpl generate_address_calculateInAnotherShard;
GenerateOpsImpl generate_AutoDeployAddress_buildAddress;
GenerateOpsImpl generate_AutoDeployAddress_calculateAddress;
GenerateOpsImpl generate_AutoDeployAddress_addressMatches;

GenerateOpsImpl generate_mapKV_exists;
GenerateOpsImpl generate_mapKV_get;
GenerateOpsImpl generate_mapKV_mustGet;
GenerateOpsImpl generate_mapKV_set;
GenerateOpsImpl generate_mapKV_setGet;
GenerateOpsImpl generate_mapKV_replace;
GenerateOpsImpl generate_mapKV_replaceGet;
GenerateOpsImpl generate_mapKV_add;
GenerateOpsImpl generate_mapKV_addGet;
GenerateOpsImpl generate_mapKV_del;
GenerateOpsImpl generate_mapKV_delGet;
GenerateOpsImpl generate_mapKV_findFirst;
GenerateOpsImpl generate_mapKV_findLast;
GenerateOpsImpl generate_mapKV_findKeyGreater;
GenerateOpsImpl generate_mapKV_findKeyGreaterOrEqual;
GenerateOpsImpl generate_mapKV_findKeyLess;
GenerateOpsImpl generate_mapKV_findKeyLessOrEqual;
GenerateOpsImpl generate_mapKV_iterateNext;
GenerateOpsImpl generate_mapKV_iteratePrev;

CompileToAsmOpImpl compile_createEmptyMap;
CompileToAsmOpImpl compile_createMapFromLowLevelDict;
CompileToAsmOpImpl compile_dict_get;
CompileToAsmOpImpl compile_dict_getMin;
CompileToAsmOpImpl compile_dict_getMax;
CompileToAsmOpImpl compile_dict_getNext;
CompileToAsmOpImpl compile_dict_getNextEq;
CompileToAsmOpImpl compile_dict_getPrev;
CompileToAsmOpImpl compile_dict_getPrevEq;
CompileToAsmOpImpl compile_dict_set;
CompileToAsmOpImpl compile_dict_setGet;
CompileToAsmOpImpl compile_dict_replace;
CompileToAsmOpImpl compile_dict_replaceGet;
CompileToAsmOpImpl compile_dict_add;
CompileToAsmOpImpl compile_dict_addGet;
CompileToAsmOpImpl compile_dict_del;
CompileToAsmOpImpl compile_dict_delGet;

void attach_builtins_implementation() {
  using namespace std::placeholders;

  TypePtr typeT = TypeDataGenericT::create("T");
  TypePtr Unit = TypeDataVoid::create();
  TypePtr Int = TypeDataInt::create();
  TypePtr Bool = TypeDataBool::create();
  TypePtr Slice = TypeDataSlice::create();
  TypePtr String = TypeDataString::create();
  TypePtr Builder = TypeDataBuilder::create();
  TypePtr Never = TypeDataNever::create();
  TypePtr Unknown = TypeDataUnknown::create();

  const GenericsDeclaration* declGenericT = new GenericsDeclaration(std::vector<GenericsDeclaration::ItemT>{{"T", nullptr}}, 0);

  std::vector ParamsInt1 = {Int};
  std::vector ParamsInt2 = {Int, Int};
  std::vector ParamsInt3 = {Int, Int, Int};

  // builtin operators
  // they are internally stored as functions, because at IR level, there is no difference
  // between calling `userAdd(a,b)` and `_+_(a,b)`
  // since they are registered in a global symtable, technically, they can even be referenced from Tolk code,
  // though it's a "hidden feature" and won't work well for overloads (`==` for int and bool, for example)

  // unary operators
  define_internal_builtin_func("-_", ParamsInt1, Int, nullptr,
                              compile_unary_minus);
  define_internal_builtin_func("+_", ParamsInt1, Int, nullptr,
                              compile_unary_plus);
  define_internal_builtin_func("!_", ParamsInt1, Bool, nullptr,
                              std::bind(compile_logical_not, _1, _2, _3, true));
  define_internal_builtin_func("!b_", {Bool}, Bool, nullptr,   // "overloaded" separate version for bool
                              std::bind(compile_logical_not, _1, _2, _3, false));
  define_internal_builtin_func("~_", ParamsInt1, Int, nullptr,
                              compile_bitwise_not);

  // binary operators
  define_internal_builtin_func("_+_", ParamsInt2, Int, nullptr,
                              compile_add);
  define_internal_builtin_func("_-_", ParamsInt2, Int, nullptr,
                              compile_sub);
  define_internal_builtin_func("_*_", ParamsInt2, Int, nullptr,
                              compile_mul);
  define_internal_builtin_func("_/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, -1));
  define_internal_builtin_func("_~/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, 0));
  define_internal_builtin_func("_^/_", ParamsInt2, Int, nullptr,
                              std::bind(compile_div, _1, _2, _3, 1));
  define_internal_builtin_func("_%_", ParamsInt2, Int, nullptr,
                              std::bind(compile_mod, _1, _2, _3, -1));
  define_internal_builtin_func("_<<_", ParamsInt2, Int, nullptr,
                              compile_lshift);
  define_internal_builtin_func("_>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, -1));
  define_internal_builtin_func("_~>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, 0));
  define_internal_builtin_func("_^>>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_rshift, _1, _2, _3, 1));
  define_internal_builtin_func("_&_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_and);
  define_internal_builtin_func("_|_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_or);
  define_internal_builtin_func("_^_", ParamsInt2, Int, nullptr,        // also works for bool
                              compile_bitwise_xor);
  define_internal_builtin_func("_==_", ParamsInt2, Int, nullptr,       // also works for bool
                              std::bind(compile_cmp_int, _1, _2, _3, 2));
  define_internal_builtin_func("_!=_", ParamsInt2, Int, nullptr,       // also works for bool
                              std::bind(compile_cmp_int, _1, _2, _3, 5));
  define_internal_builtin_func("_<_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 4));
  define_internal_builtin_func("_>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 1));
  define_internal_builtin_func("_<=_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 6));
  define_internal_builtin_func("_>=_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 3));
  define_internal_builtin_func("_<=>_", ParamsInt2, Int, nullptr,
                              std::bind(compile_cmp_int, _1, _2, _3, 7));

  // special function used for internal compilation of some lexical constructs
  // for example, `throw 123;` is actually calling `__throw(123)`
  define_internal_builtin_func("__true", {}, Bool, nullptr, /* AsmOp::Const("TRUE") */
                              std::bind(compile_bool_const, _1, _2, _3, true));
  define_internal_builtin_func("__false", {}, Bool, nullptr, /* AsmOp::Const("FALSE") */
                              std::bind(compile_bool_const, _1, _2, _3, false));
  define_internal_builtin_func("__null", {}, Unknown, nullptr,
                              compile_push_null);
  define_internal_builtin_func("__isNull", {Unknown}, Bool, nullptr,
                              compile_is_null);
  define_internal_builtin_func("__throw", ParamsInt1, Never, nullptr,
                              compile_throw,
                                0);
  define_internal_builtin_func("__throw_arg", {Unknown, Int}, Never, nullptr,
                              compile_throw_arg,
                                0);
  define_internal_builtin_func("__throw_if", ParamsInt2, Unit, nullptr,
                              std::bind(compile_throw_if_ifnot, _1, _2, _3, false),
                                0);
  define_internal_builtin_func("__throw_ifnot", ParamsInt2, Unit, nullptr,
                              std::bind(compile_throw_if_ifnot, _1, _2, _3, true),
                                0);
  define_internal_builtin_func("__InMessage.originalForwardFee", ParamsInt2, Int, nullptr,
                                compile_calc_InMessage_originalForwardFee);
  define_internal_builtin_func("__InMessage.getInMsgParam", ParamsInt1, Int, nullptr,
                                compile_calc_InMessage_getInMsgParam);
  define_internal_builtin_func("__storeVarInt", {Builder, Int, Int, Bool}, Builder, nullptr,
                               compile_store_varint);   // not exposed to stdlib, used in auto-serialization
  define_internal_builtin_func("__loadVarInt", {Slice, Int, Bool}, TypeDataTensor::create({Int, Slice}), nullptr,
                               compile_fetch_varint);   // not exposed to stdlib, used in auto-serialization
  define_internal_builtin_func("__condsel", ParamsInt3, Int, nullptr,
                              compile_ternary_as_condsel);
  define_internal_builtin_func("__tryStripPrefix", {Slice, Int, Int}, TypeDataTensor::create({Slice, Bool}), nullptr,
                               compile_slice_sdbeginsq);

  // functions not presented in stdlib at all
  // used in tolk-tester to check/expose internal compiler state
  // each of them is handled in a special way, search by its name
  define_internal_builtin_func("__expect_type", {typeT, String}, Unit, declGenericT,
                                compile_expect_type);
  define_internal_builtin_func("__expect_inline", {Bool}, Unit, nullptr,
                                compile_expect_type);
  define_internal_builtin_func("__expect_lazy", {String}, Unit, nullptr,
                                compile_expect_type);

  // low-level functions that actually emit TVM assembly, they work on a "dict" level
  TypePtr PlainDict = TypeDataCell::create();
  TypePtr KeySliceOrInt = TypeDataUnknown::create();
  TypePtr ValueSlice = TypeDataSlice::create();
  TypePtr ValueFound = TypeDataInt::create();
  TypePtr LookupSliceFound = TypeDataTensor::create({TypeDataSlice::create(), TypeDataInt::create()});
  define_internal_builtin_func("__dict.get", {KeySliceOrInt, PlainDict, TypeDataInt::create()}, LookupSliceFound,
                               nullptr, compile_dict_get);
  define_internal_builtin_func("__dict.getMin", {PlainDict},
                               TypeDataTensor::create({ValueSlice, KeySliceOrInt, ValueFound}), nullptr,
                               compile_dict_getMin);
  define_internal_builtin_func("__dict.getMax", {PlainDict},
                               TypeDataTensor::create({ValueSlice, KeySliceOrInt, ValueFound}), nullptr,
                               compile_dict_getMax);
  define_internal_builtin_func("__dict.getNext",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr,
                               compile_dict_getNext);
  define_internal_builtin_func("__dict.getNextEq",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr,
                               compile_dict_getNextEq);
  define_internal_builtin_func("__dict.getPrev",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr,
                               compile_dict_getPrev);
  define_internal_builtin_func("__dict.getPrevEq",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr,
                               compile_dict_getPrevEq);
  define_internal_builtin_func("__dict.set", {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               PlainDict, nullptr, compile_dict_set);
  define_internal_builtin_func("__dict.setGet",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, LookupSliceFound}), nullptr, compile_dict_setGet);
  define_internal_builtin_func("__dict.replace",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr, compile_dict_replace);
  define_internal_builtin_func("__dict.replaceGet",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, LookupSliceFound}), nullptr, compile_dict_replaceGet);
  define_internal_builtin_func("__dict.add", {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr, compile_dict_add);
  define_internal_builtin_func("__dict.addGet",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, LookupSliceFound}), nullptr, compile_dict_addGet);
  define_internal_builtin_func("__dict.del", {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, TypeDataBool::create()}), nullptr, compile_dict_del);
  define_internal_builtin_func("__dict.delGet",
                               {KeySliceOrInt, TypeDataSlice::create(), PlainDict, TypeDataInt::create()},
                               TypeDataTensor::create({PlainDict, LookupSliceFound}), nullptr, compile_dict_delGet);

  // compile-time only functions, evaluated essentially at compile-time, no runtime implementation
  // they are placed in stdlib and marked as `builtin`
  // note their parameter being `unknown`: in order to `grams(1)` pass type inferring but fire a more gentle error later
  redefine_builtin_body("grams", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("ton", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringCrc32", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringCrc16", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringSha256", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringSha256_32", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringToBase256", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("stringHexToSlice", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("address", compile_time_only_function, FunctionData::flagCompileTimeVal);

  // string compile-time methods: "hello".crc32(), "hello".sha256(), etc.
  redefine_builtin_body("string.crc32", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.crc16", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.sha256", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.sha256_32", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.hexToSlice", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.toBase256", compile_time_only_function, FunctionData::flagCompileTimeVal);
  redefine_builtin_body("string.literalSlice", compile_time_only_function, FunctionData::flagCompileTimeVal);

  // array<T> — a TVM tuple under the hood
  // implemented as built-in functions to support variable-width T (not 1-slot values are backed by sub-tuples)
  redefine_builtin_body("array<T>.get", compile_array_get);
  redefine_builtin_body("array<T>.set", compile_array_set_at);
  redefine_builtin_body("array<T>.push", compile_array_push);
  redefine_builtin_body("array<T>.size", compile_array_size);
  redefine_builtin_body("array<T>.last", compile_array_last);
  redefine_builtin_body("array<T>.first", compile_array_first);
  redefine_builtin_body("array<T>.pop", compile_array_pop);

  // functions from stdlib with illustrative asm bodies, implemented at compiler level for optimizations
  // (for example, `loadInt(1)` is `1 LDI`, but `loadInt(n)` for non-constant requires it be on a stack and `LDIX`)
  redefine_builtin_body("mulDivFloor", std::bind(compile_muldiv, _1, _2, _3, -1));
  redefine_builtin_body("mulDivRound", std::bind(compile_muldiv, _1, _2, _3, 0));
  redefine_builtin_body("mulDivCeil", std::bind(compile_muldiv, _1, _2, _3, 1));
  redefine_builtin_body("slice.loadInt", std::bind(compile_fetch_int, _1, _2, _3, true, true));
  redefine_builtin_body("slice.loadUint", std::bind(compile_fetch_int, _1, _2, _3, true, false));
  redefine_builtin_body("slice.loadBits", std::bind(compile_fetch_slice, _1, _2, _3, true));
  redefine_builtin_body("slice.skipBits", compile_skip_bits_in_slice);
  redefine_builtin_body("slice.preloadInt", std::bind(compile_fetch_int, _1, _2, _3, false, true));
  redefine_builtin_body("slice.preloadUint", std::bind(compile_fetch_int, _1, _2, _3, false, false));
  redefine_builtin_body("slice.preloadBits", std::bind(compile_fetch_slice, _1, _2, _3, false));
  redefine_builtin_body("builder.storeInt", std::bind(compile_store_int, _1, _2, _3, true));
  redefine_builtin_body("builder.storeUint", std::bind(compile_store_int, _1, _2, _3, false));
  redefine_builtin_body("builder.storeBool", compile_store_bool);
  redefine_builtin_body("builder.storeCoins", compile_store_coins);
  redefine_builtin_body("address.buildSameAddressInAnotherShard", generate_address_buildInAnotherShard);
  redefine_builtin_body("address.calculateSameAddressInAnotherShard", generate_address_calculateInAnotherShard);
  redefine_builtin_body("debug.print", compile_debug_print_to_string);

  // reflect — compile-time type introspection;
  // a couple of its methods are "consteval" and can be used in constants / fields defaults / etc.
  if (lookup_global_symbol("reflect")) {
    redefine_builtin_body("reflect.typeNameOf", compile_time_only_function, FunctionData::flagCompileTimeVal);
    redefine_builtin_body("reflect.typeNameOfObject", compile_time_only_function, FunctionData::flagCompileTimeVal);
    redefine_builtin_body("reflect.typeUniqueIdxOf", compile_time_only_function, FunctionData::flagCompileTimeVal);
    redefine_builtin_body("reflect.typeUniqueIdxOfObject", compile_time_only_function, FunctionData::flagCompileTimeVal);
    redefine_builtin_body("reflect.stackSizeOf", generate_reflect_stackSizeOf);
    redefine_builtin_body("reflect.stackSizeOfObject", compile_reflect_stackSizeOfObject);
    redefine_builtin_body("reflect.serializationPrefixOf", generate_reflect_serializationPrefixOf);
    redefine_builtin_body("reflect.estimateSerializationOf", generate_reflect_estimateSerializationOf);
    redefine_builtin_body("reflect.sourceLocation", compile_time_only_function, FunctionData::flagCompileTimeVal);
    redefine_builtin_body("reflect.sourceLocationAsString", compile_time_only_function, FunctionData::flagCompileTimeVal);
  }

  // serialization/deserialization methods to/from cells (or, more low-level, slices/builders)
  // they work with structs (or, more low-level, with arbitrary types)
  redefine_builtin_body("T.toCell", generate_T_toCell);
  redefine_builtin_body("T.fromCell", generate_T_fromCell);
  redefine_builtin_body("T.fromSlice", generate_T_fromSlice);
  redefine_builtin_body("T.forceLoadLazyObject", generate_T_forceLoadLazyObject);
  redefine_builtin_body("Cell<T>.load", generate_T_fromCell);
  redefine_builtin_body("slice.loadAny", generate_slice_loadAny);
  redefine_builtin_body("slice.skipAny", generate_slice_skipAny);
  redefine_builtin_body("builder.storeAny", generate_builder_storeAny);
  redefine_builtin_body("T.toTuple", compile_T_to_tuple);
  redefine_builtin_body("T.fromTuple", compile_T_from_tuple);

  redefine_builtin_body("createMessage", generate_createMessage);
  redefine_builtin_body("createExternalLogMessage", generate_createExternalLogMessage);
  redefine_builtin_body("AutoDeployAddress.buildAddress", generate_AutoDeployAddress_buildAddress);
  redefine_builtin_body("AutoDeployAddress.calculateAddress", generate_AutoDeployAddress_calculateAddress);
  redefine_builtin_body("AutoDeployAddress.addressMatches", generate_AutoDeployAddress_addressMatches);

  // high-level methods for maps;
  // they are generic, so all type checks are done automatically;
  // but all calls to them are handled at generating Ops from AST, their "simple compile" is not called
  redefine_builtin_body("createEmptyMap", compile_createEmptyMap);
  redefine_builtin_body("createMapFromLowLevelDict", compile_createMapFromLowLevelDict);
  redefine_builtin_body("map<K, V>.exists", generate_mapKV_exists);
  redefine_builtin_body("map<K, V>.get", generate_mapKV_get);
  redefine_builtin_body("map<K, V>.mustGet", generate_mapKV_mustGet);
  redefine_builtin_body("map<K, V>.set", generate_mapKV_set);
  redefine_builtin_body("map<K, V>.setAndGetPrevious", generate_mapKV_setGet);
  redefine_builtin_body("map<K, V>.replaceIfExists", generate_mapKV_replace);
  redefine_builtin_body("map<K, V>.replaceAndGetPrevious", generate_mapKV_replaceGet);
  redefine_builtin_body("map<K, V>.addIfNotExists", generate_mapKV_add);
  redefine_builtin_body("map<K, V>.addOrGetExisting", generate_mapKV_addGet);
  redefine_builtin_body("map<K, V>.delete", generate_mapKV_del);
  redefine_builtin_body("map<K, V>.deleteAndGetDeleted", generate_mapKV_delGet);
  redefine_builtin_body("map<K, V>.findFirst", generate_mapKV_findFirst);
  redefine_builtin_body("map<K, V>.findLast", generate_mapKV_findLast);
  redefine_builtin_body("map<K, V>.findKeyGreater", generate_mapKV_findKeyGreater);
  redefine_builtin_body("map<K, V>.findKeyGreaterOrEqual", generate_mapKV_findKeyGreaterOrEqual);
  redefine_builtin_body("map<K, V>.findKeyLess", generate_mapKV_findKeyLess);
  redefine_builtin_body("map<K, V>.findKeyLessOrEqual", generate_mapKV_findKeyLessOrEqual);
  redefine_builtin_body("map<K, V>.iterateNext", generate_mapKV_iterateNext);
  redefine_builtin_body("map<K, V>.iteratePrev", generate_mapKV_iteratePrev);
}

}  // namespace tolk
