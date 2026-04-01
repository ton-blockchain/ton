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

#include "compilation-errors.h"
#include "crypto/common/refint.h"
#include <functional>
#include <vector>
#include <string>
#include <stack>

/*
    This file is "inherited from FunC" (keep in mind, that Tolk is a fork of FunC compiler, gradually improved)
  and contains "uncategorized" entities — almost unchanged from FunC.
    Some day, when intermediate representation (IR, aka Ops) is fully rewritten, it will be removed.
 */

namespace tolk {


/*
 * 
 *   ABSTRACT CODE
 * 
 */

struct TmpVar {
  var_idx_t ir_idx;   // every var in IR represents 1 stack slot
  TypePtr v_type;     // get_width_on_stack() is 1
  std::string name;   // "x" for vars originated from user sources; "x.0" for tensor components; empty for implicitly created tmp vars
#ifdef TOLK_DEBUG
  const char* purpose = nullptr; // "purpose" of tmp var, for debug output like `'15 (binary-op) '16 (glob-var)`
#endif

  TmpVar(var_idx_t ir_idx, TypePtr v_type, std::string name)
    : ir_idx(ir_idx)
    , v_type(v_type)
    , name(std::move(name)) {
  }
};

struct VarDescr {
  var_idx_t idx;
  enum { _Last = 1, _Unused = 2 };
  int flags;
  enum {
    _Const = 16,
    _Int = 32,
    _Zero = 64,
    _NonZero = 128,
    _Pos = 256,
    _Neg = 512,
    _Bool = 1024,
    _Bit = 2048,
    _Finite = 4096,
    _Nan = 8192,
    _Even = 16384,
    _Odd = 32768,
    _Null = (1 << 16),
    _NotNull = (1 << 17)
  };
  static constexpr int ConstZero = _Int | _Zero | _Pos | _Neg | _Bool | _Bit | _Finite | _Even | _NotNull;
  static constexpr int ConstOne = _Int | _NonZero | _Pos | _Bit | _Finite | _Odd | _NotNull;
  static constexpr int ConstTrue = _Int | _NonZero | _Neg | _Bool | _Finite | _Odd | _NotNull;
  static constexpr int ValBit = ConstZero & ConstOne;
  static constexpr int ValBool = ConstZero & ConstTrue;
  static constexpr int FiniteInt = _Int | _Finite | _NotNull;
  static constexpr int FiniteUInt = FiniteInt | _Pos;
  int val;
  td::RefInt256 int_const;

  explicit VarDescr(var_idx_t _idx = -1, int _flags = 0, int _val = 0) : idx(_idx), flags(_flags), val(_val) {
  }
  bool operator<(var_idx_t other_idx) const {
    return idx < other_idx;
  }
  bool is_unused() const {
    return flags & _Unused;
  }
  bool is_last() const {
    return flags & _Last;
  }
  bool always_true() const {
    return val & _NonZero;
  }
  bool always_false() const {
    return val & _Zero;
  }
  bool always_nonzero() const {
    return val & _NonZero;
  }
  bool always_zero() const {
    return val & _Zero;
  }
  bool always_even() const {
    return val & _Even;
  }
  bool always_odd() const {
    return val & _Odd;
  }
  bool always_null() const {
    return val & _Null;
  }
  bool always_not_null() const {
    return val & _NotNull;
  }
  bool is_int_const() const {
    return (val & (_Int | _Const)) == (_Int | _Const) && int_const.not_null();
  }
  bool always_nonpos() const {
    return val & _Neg;
  }
  bool always_nonneg() const {
    return val & _Pos;
  }
  bool always_pos() const {
    return (val & (_Pos | _NonZero)) == (_Pos | _NonZero);
  }
  bool always_neg() const {
    return (val & (_Neg | _NonZero)) == (_Neg | _NonZero);
  }
  bool always_finite() const {
    return val & _Finite;
  }
  bool always_less(const VarDescr& other) const;
  bool always_leq(const VarDescr& other) const;
  bool always_greater(const VarDescr& other) const;
  bool always_geq(const VarDescr& other) const;
  bool always_equal(const VarDescr& other) const;
  bool always_neq(const VarDescr& other) const;
  void unused() {
    flags |= _Unused;
  }
  void clear_unused() {
    flags &= ~_Unused;
  }
  void set_const(long long value);
  void set_const(td::RefInt256 value);
  void set_const(const std::string& value);
  void operator+=(const VarDescr& y) {
    flags &= y.flags;
  }
  void operator|=(const VarDescr& y);
  void operator&=(const VarDescr& y);
  void set_value(const VarDescr& y);
  void set_value(VarDescr&& y);
  void set_value(const VarDescr* y) {
    if (y) {
      set_value(*y);
    }
  }
  void clear_value();
  void show_value(std::ostream& os) const;
  void show(std::ostream& os, const char* var_name = nullptr) const;
};

inline std::ostream& operator<<(std::ostream& os, const VarDescr& vd) {
  vd.show(os);
  return os;
}

struct VarDescrList {
  std::vector<VarDescr> list;
  bool unreachable{false};
  VarDescrList() : list() {
  }
  VarDescrList(const std::vector<VarDescr>& _list) : list(_list) {
  }
  VarDescrList(std::vector<VarDescr>&& _list) : list(std::move(_list)) {
  }
  std::size_t size() const {
    return list.size();
  }
  VarDescr* operator[](var_idx_t idx);
  const VarDescr* operator[](var_idx_t idx) const;
  VarDescrList operator+(const VarDescrList& y) const;
  VarDescrList& operator+=(const VarDescrList& y);
  VarDescrList& clear_last();
  VarDescrList& operator+=(var_idx_t idx) {
    return add_var(idx);
  }
  VarDescrList& operator+=(const std::vector<var_idx_t>& idx_list) {
    return add_vars(idx_list);
  }
  VarDescrList& add_var(var_idx_t idx, bool unused = false);
  VarDescrList& add_vars(const std::vector<var_idx_t>& idx_list, bool unused = false);
  VarDescrList& operator-=(const std::vector<var_idx_t>& idx_list);
  VarDescrList& operator-=(var_idx_t idx);
  std::size_t count(const std::vector<var_idx_t> idx_list) const;
  std::size_t count_used(const std::vector<var_idx_t> idx_list) const;
  VarDescr& add(var_idx_t idx);
  VarDescr& add_newval(var_idx_t idx);
  VarDescrList& import_values(const VarDescrList& values);
  VarDescrList operator|(const VarDescrList& y) const;
  VarDescrList& operator|=(const VarDescrList& values);
  void show(std::ostream& os) const;
  void set_unreachable() {
    list.clear();
    unreachable = true;
  }
};

inline std::ostream& operator<<(std::ostream& os, const VarDescrList& values) {
  values.show(os);
  return os;
}

struct CodeBlob;

struct Stack;

struct Op;

struct OpList {
  std::vector<std::unique_ptr<Op>> list;

  // container interface (forwarding to inner vector)
  bool empty() const { return list.empty(); }
  size_t size() const { return list.size(); }
  std::unique_ptr<Op>& operator[](size_t i) { return list[i]; }
  const std::unique_ptr<Op>& operator[](size_t i) const { return list[i]; }
  const std::unique_ptr<Op>& front() const { return list.front(); }
  std::unique_ptr<Op>& back() { return list.back(); }
  void clear() { list.clear(); }

  Op& push_back(std::unique_ptr<Op> op) {
    list.push_back(std::move(op));
    return *list.back();
  }
  template<typename... Args>
  Op& push_back(Args&&... args) {
    list.push_back(std::make_unique<Op>(std::forward<Args>(args)...));
    return *list.back();
  }

  using iterator = std::vector<std::unique_ptr<Op>>::iterator;
  using const_iterator = std::vector<std::unique_ptr<Op>>::const_iterator;
  iterator begin() { return list.begin(); }
  iterator end() { return list.end(); }
  const_iterator begin() const { return list.begin(); }
  const_iterator end() const { return list.end(); }
  iterator insert(const_iterator pos, std::unique_ptr<Op> op) { return list.insert(pos, std::move(op)); }

  // semantic accessors (defined after Op, since they access Op's members)
  static std::unique_ptr<Op> make_terminal_nop(AnyV origin);
  bool is_noreturn() const;
  bool is_empty_block() const;
  const VarDescrList& entry_var_info() const;
  const VarDescrList& exit_var_info() const;

  // IR analysis and codegen (defined in respective .cpp files)
  void set_entry_var_info(VarDescrList&& front_var_info);
  void generate_code_all(Stack& stack, size_t from = 0) const;
  bool compute_used_code_vars(const VarDescrList& var_info, bool edit);
  VarDescrList fwd_analyze(VarDescrList values) const;
  bool mark_noreturn();
  bool prune_unreachable();
  void mark_function_used_dfs() const;
  void show(std::ostream& os, const std::vector<TmpVar>& vars, const std::string& indent, int mode = 0) const;
};

struct Op {
  enum OpKind {
    _Nop,
    _Call,
    _CallInd,
    _Let,
    _IntConst,
    _GlobVar,
    _SetGlob,
    _Import,
    _Return,
    _Tuple,
    _UnTuple,
    _If,
    _While,
    _Until,
    _Repeat,
    _Again,
    _TryCatch,
    _SliceConst,
    _SnakeStringConst,
  };
  OpKind cl;
  enum { _Disabled = 1, _NoReturn = 2, _Impure = 4, _ArgOrderAlreadyEqualsAsm = 8 };
  int flags;
  FunctionPtr f_sym = nullptr;
  GlobalVarPtr g_sym = nullptr;
  AnyV origin;
  VarDescrList var_info;
  std::vector<VarDescr> args;
  std::vector<var_idx_t> left, right;
  OpList block0, block1;
  td::RefInt256 int_const;
  std::string str_const;
  Op(AnyV origin, OpKind cl, std::vector<var_idx_t> left = {}) : cl(cl), flags(0), origin(origin), left(std::move(left)) {
  }

  static std::unique_ptr<Op> make_let(AnyV origin, std::vector<var_idx_t> dst, std::vector<var_idx_t> src) {
    auto op = std::make_unique<Op>(origin, _Let, std::move(dst));
    op->right = std::move(src);
    return op;
  }

  bool disabled() const { return flags & _Disabled; }
  void set_disabled() { flags |= _Disabled; }
  void set_disabled(bool flag);

  bool noreturn() const { return flags & _NoReturn; }
  bool set_noreturn() { flags |= _NoReturn; return true; }
  bool set_noreturn(bool flag);

  bool impure() const { return flags & _Impure; }
  void set_impure_flag();

  bool arg_order_already_equals_asm() const { return flags & _ArgOrderAlreadyEqualsAsm; }
  void set_arg_order_already_equals_asm_flag();

  void show(std::ostream& os, const std::vector<TmpVar>& vars, const std::string& indent, int mode = 0) const;
  void show_var_list(std::ostream& os, const std::vector<var_idx_t>& idx_list, const std::vector<TmpVar>& vars) const;
  void show_var_list(std::ostream& os, const std::vector<VarDescr>& list, const std::vector<TmpVar>& vars) const;
  bool compute_used_vars(bool edit, const VarDescrList& next_var_info);
  bool std_compute_used_vars(const VarDescrList& next_var_info, bool disabled = false);
  bool set_var_info(const VarDescrList& new_var_info);
  bool set_var_info(VarDescrList&& new_var_info);
  bool set_var_info_except(const VarDescrList& new_var_info, const std::vector<var_idx_t>& var_list);
  bool set_var_info_except(VarDescrList&& new_var_info, const std::vector<var_idx_t>& var_list);
  void prepare_args(VarDescrList values);
  void maybe_swap_builtin_args_to_compile();
  VarDescrList fwd_analyze(VarDescrList values);
  bool generate_code_step(Stack& stack, const OpList& parent_ops, size_t self_idx);
};

// OpList inline methods that need Op to be complete
inline std::unique_ptr<Op> OpList::make_terminal_nop(AnyV origin) {
  return std::make_unique<Op>(origin, Op::_Nop);
}
inline bool OpList::is_noreturn() const {
  return !list.empty() && list.front()->noreturn();
}
inline bool OpList::is_empty_block() const {
  return list.empty() || (list.size() == 1 && list.front()->cl == Op::_Nop);
}
inline const VarDescrList& OpList::entry_var_info() const {
  return list.front()->var_info;
}
inline const VarDescrList& OpList::exit_var_info() const {
  return list.back()->var_info;
}

struct FunctionBodyCode {
  CodeBlob* code = nullptr;
  void set_code(CodeBlob* code);
};

/*
 * 
 *   GENERATE CODE
 * 
 */

struct AsmOp {
  enum Type { a_nop, a_xchg, a_push, a_pop, a_const, a_comment, a_custom };
  Type t;
  AnyV origin;
  int a, b;
  bool gconst{false};
  std::string op;
  struct SReg {
    int idx;
    explicit SReg(int _idx) : idx(_idx) {
    }
    int calc_out_strlen() const;
  };
  AsmOp() = default;
  AsmOp(Type t, AnyV origin) : t(t), origin(origin) {
  }
  AsmOp(Type t, AnyV origin, std::string _op) : t(t), origin(origin), op(std::move(_op)) {
  }
  AsmOp(Type t, AnyV origin, int a) : t(t), origin(origin), a(a) {
  }
  AsmOp(Type t, AnyV origin, int a, std::string _op) : t(t), origin(origin), a(a), op(std::move(_op)) {
  }
  AsmOp(Type t, AnyV origin, int a, int b) : t(t), origin(origin), a(a), b(b) {
  }
  AsmOp(Type t, AnyV origin, int a, int b, std::string op) : t(t), origin(origin), a(a), b(b), op(std::move(op)) {
    compute_gconst();
  }
  int out(std::ostream& os) const;
  void output_to_fif(std::ostream& os, int indent, bool print_comment_slashes) const;
  void compute_gconst() {
    gconst = (is_custom() && (op == "PUSHNULL" || op == "NEWC" || op == "NEWB" || op == "TRUE" || op == "FALSE" || op == "NOW"));
  }
  bool is_nop() const {
    return t == a_nop;
  }
  bool is_custom() const {
    return t == a_custom;
  }
  bool is_very_custom() const {
    return is_custom() && a >= 255;
  }
  bool is_comment() const {
    return t == a_comment;
  }
  bool is_push() const {
    return t == a_push;
  }
  bool is_push(int x) const {
    return is_push() && a == x;
  }
  bool is_push(int* x) const {
    *x = a;
    return is_push();
  }
  bool is_pop() const {
    return t == a_pop;
  }
  bool is_pop(int x) const {
    return is_pop() && a == x;
  }
  bool is_xchg() const {
    return t == a_xchg;
  }
  bool is_xchg(int x, int y) const {
    return is_xchg() && b == y && a == x;
  }
  bool is_xchg(int* x, int* y) const {
    *x = a;
    *y = b;
    return is_xchg();
  }
  bool is_xchg_short() const {
    return is_xchg() && (a <= 1 || b <= 1);
  }
  bool is_swap() const {
    return is_xchg(0, 1);
  }
  bool is_const() const {
    return t == a_const && !a && b == 1;
  }
  bool is_gconst() const {
    return !a && b == 1 && (t == a_const || gconst);
  }
  static AsmOp Nop(AnyV origin) {
    return AsmOp(a_nop, origin);
  }
  static AsmOp Xchg(AnyV origin, int a, int b = 0) {
    return a == b ? AsmOp(a_nop, origin) : (a < b ? AsmOp(a_xchg, origin, a, b) : AsmOp(a_xchg, origin, b, a));
  }
  static AsmOp Push(AnyV origin, int a) {
    return AsmOp(a_push, origin, a);
  }
  static AsmOp Pop(AnyV origin, int a) {
    return AsmOp(a_pop, origin, a);
  }
  static AsmOp Xchg2(AnyV origin, int a, int b) {
    return make_stk2(origin, a, b, "XCHG2", 0);
  }
  static AsmOp XcPu(AnyV origin, int a, int b) {
    return make_stk2(origin, a, b, "XCPU", 1);
  }
  static AsmOp PuXc(AnyV origin, int a, int b) {
    return make_stk2(origin, a, b, "PUXC", 1);
  }
  static AsmOp Push2(AnyV origin, int a, int b) {
    return make_stk2(origin, a, b, "PUSH2", 2);
  }
  static AsmOp Xchg3(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "XCHG3", 0);
  }
  static AsmOp Xc2Pu(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "XC2PU", 1);
  }
  static AsmOp XcPuXc(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "XCPUXC", 1);
  }
  static AsmOp XcPu2(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "XCPU2", 3);
  }
  static AsmOp PuXc2(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "PUXC2", 3);
  }
  static AsmOp PuXcPu(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "PUXCPU", 3);
  }
  static AsmOp Pu2Xc(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "PU2XC", 3);
  }
  static AsmOp Push3(AnyV origin, int a, int b, int c) {
    return make_stk3(origin, a, b, c, "PUSH3", 3);
  }
  static AsmOp BlkSwap(AnyV origin, int a, int b);
  static AsmOp BlkPush(AnyV origin, int a, int b);
  static AsmOp BlkDrop(AnyV origin, int a);
  static AsmOp BlkDrop2(AnyV origin, int a, int b);
  static AsmOp BlkReverse(AnyV origin, int a, int b);
  static AsmOp make_stk2(AnyV origin, int a, int b, const char* str, int delta);
  static AsmOp make_stk3(AnyV origin, int a, int b, int c, const char* str, int delta);
  static AsmOp IntConst(AnyV origin, const td::RefInt256& x);
  static AsmOp BoolConst(AnyV origin, bool f);
  static AsmOp Const(AnyV origin, std::string push_op) {
    return AsmOp(a_const, origin, 0, 1, std::move(push_op));
  }
  static AsmOp Const(AnyV origin, int arg, const std::string& push_op);
  static AsmOp Custom(AnyV origin, std::string custom_op) {
    return AsmOp(a_custom, origin, 255, 255, std::move(custom_op));
  }
  static AsmOp Custom(AnyV origin, std::string custom_op, int args, int retv = 1) {
    return AsmOp(a_custom, origin, args, retv, std::move(custom_op));
  }
  static AsmOp Parse(AnyV origin, const std::string& custom_op);
  static AsmOp Parse(AnyV origin, std::string custom_op, int args, int retv = 1);
  static AsmOp Tuple(AnyV origin, int a);
  static AsmOp UnTuple(AnyV origin, int a);
  static AsmOp Comment(AnyV origin, std::string comment) {
    return AsmOp(a_comment, origin, 255, 255, std::move(comment));
  }
};

std::ostream& operator<<(std::ostream& os, AsmOp::SReg stack_reg);

struct AsmOpList {
  std::vector<AsmOp> list_;
  bool retalt_{false};
  bool retalt_inserted_{false};
  void operator<<(AsmOp&& op) {
    if (!op.is_nop()) {
      list_.emplace_back(op);
    }
  }
  void insert(size_t pos, AnyV origin, std::string str) {
    list_.insert(list_.begin() + pos, AsmOp::Custom(origin, std::move(str)));
  }
};

int is_pos_pow2(td::RefInt256 x);
int is_neg_pow2(td::RefInt256 x);

/*
 * 
 *  STACK TRANSFORMS
 * 
 */

/*
A stack transform is a map f:N={0,1,...} -> N, such that f(x) = x + d_f for almost all x:N and for a fixed d_f:N.
They form a monoid under composition: (fg)(x)=f(g(x)).
They act on stacks S on the right: Sf=S', such that S'[n]=S[f(n)].

A stack transform f is determined by d_f and the finite set A of all pairs (x,y), such that x>=d_f, f(x-d_f) = y and y<>x. They are listed in increasing order by x.
*/
struct StackTransform {
  enum { max_n = 16, inf_x = 0x7fffffff, c_start = -1000 };
  int d{0}, n{0}, dp{0}, c{0};
  bool invalid{false};
  std::array<std::pair<short, short>, max_n> A;
  StackTransform() = default;
  // list of f(0),f(1),...,f(s); assumes next values are f(s)+1,f(s)+2,...
  StackTransform(std::initializer_list<int> list);
  StackTransform& operator=(std::initializer_list<int> list);
  bool assign(const StackTransform& other);
  static StackTransform id() {
    return {};
  }
  bool invalidate() {
    invalid = true;
    return false;
  }
  bool is_valid() const {
    return !invalid;
  }
  bool set_id() {
    d = n = dp = c = 0;
    invalid = false;
    return true;
  }
  bool shift(int offs) {  // post-composes with x -> x + offs
    d += offs;
    return offs <= 0 || remove_negative();
  }
  bool remove_negative();
  bool touch(int i) {
    dp = std::max(dp, i + d + 1);
    return true;
  }
  bool is_permutation() const;         // is f:N->N bijective ?
  bool is_trivial_after(int x) const;  // f(x') = x' + d for all x' >= x
  int preimage_count(int y) const;     // card f^{-1}(y)
  std::vector<int> preimage(int y) const;
  bool apply_xchg(int i, int j, bool relaxed = false);
  bool apply_push(int i);
  bool apply_pop(int i = 0);
  bool apply_push_newconst();
  bool apply_blkpop(int k);
  bool apply(const StackTransform& other);     // this = this * other
  bool preapply(const StackTransform& other);  // this = other * this
  // c := a * b
  static bool compose(const StackTransform& a, const StackTransform& b, StackTransform& c);
  StackTransform& operator*=(const StackTransform& other);
  StackTransform operator*(const StackTransform& b) const &;
  bool equal(const StackTransform& other, bool relaxed = false) const;
  bool almost_equal(const StackTransform& other) const {
    return equal(other, true);
  }
  bool operator<=(const StackTransform& other) const {
    return dp <= other.dp && almost_equal(other);
  }
  bool operator>=(const StackTransform& other) const {
    return dp >= other.dp && almost_equal(other);
  }
  int get(int i) const;
  int touch_get(int i, bool relaxed = false) {
    if (!relaxed) {
      touch(i);
    }
    return get(i);
  }
  bool set(int i, int v, bool relaxed = false);
  static const StackTransform rot;
  static const StackTransform rot_rev;
  bool is_id() const {
    return is_valid() && !d && !n;
  }
  bool is_xchg(int i, int j) const;
  bool is_xchg(int* i, int* j) const;
  bool is_xchg_xchg(int i, int j, int k, int l) const;
  bool is_xchg_xchg(int* i, int* j, int* k, int* l) const;
  bool is_push(int i) const;
  bool is_push(int* i) const;
  bool is_pop(int i) const;
  bool is_pop(int* i) const;
  bool is_pop_pop(int i, int j) const;
  bool is_pop_pop(int* i, int* j) const;
  bool is_rot() const;
  bool is_rotrev() const;
  bool is_push_rot(int i) const;
  bool is_push_rot(int* i) const;
  bool is_push_rotrev(int i) const;
  bool is_push_rotrev(int* i) const;
  bool is_push_xchg(int i, int j, int k) const;
  bool is_push_xchg(int* i, int* j, int* k) const;
  bool is_xchg2(int i, int j) const;
  bool is_xchg2(int* i, int* j) const;
  bool is_xcpu(int i, int j) const;
  bool is_xcpu(int* i, int* j) const;
  bool is_puxc(int i, int j) const;
  bool is_puxc(int* i, int* j) const;
  bool is_push2(int i, int j) const;
  bool is_push2(int* i, int* j) const;
  bool is_xchg3(int* i, int* j, int* k) const;
  bool is_xc2pu(int* i, int* j, int* k) const;
  bool is_xcpuxc(int* i, int* j, int* k) const;
  bool is_xcpu2(int* i, int* j, int* k) const;
  bool is_puxc2(int i, int j, int k) const;
  bool is_puxc2(int* i, int* j, int* k) const;
  bool is_puxcpu(int* i, int* j, int* k) const;
  bool is_pu2xc(int i, int j, int k) const;
  bool is_pu2xc(int* i, int* j, int* k) const;
  bool is_push3(int i, int j, int k) const;
  bool is_push3(int* i, int* j, int* k) const;
  bool is_blkswap(int i, int j) const;
  bool is_blkswap(int* i, int* j) const;
  bool is_blkpush(int i, int j) const;
  bool is_blkpush(int* i, int* j) const;
  bool is_blkdrop(int* i) const;
  bool is_blkdrop2(int i, int j) const;
  bool is_blkdrop2(int* i, int* j) const;
  bool is_reverse(int i, int j) const;
  bool is_reverse(int* i, int* j) const;
  bool is_nip_seq(int i, int j = 0) const;
  bool is_nip_seq(int* i) const;
  bool is_nip_seq(int* i, int* j) const;
  bool is_pop_blkdrop(int i, int k) const;
  bool is_pop_blkdrop(int* i, int* k) const;
  bool is_2pop_blkdrop(int i, int j, int k) const;
  bool is_2pop_blkdrop(int* i, int* j, int* k) const;
  bool is_const_rot(int c) const;
  bool is_const_rot(int* c) const;
  bool is_const_pop(int c, int i) const;
  bool is_const_pop(int* c, int* i) const;
  bool is_push_const(int i, int c) const;
  bool is_push_const(int* i, int* c) const;

  void show(std::ostream& os, int mode = 0) const;

  static StackTransform Xchg(int i, int j, bool relaxed = false);

 private:
  int try_load(int& i, int offs = 0) const;  // returns A[i++].first + offs or inf_x
  bool try_store(int x, int y);              // appends (x,y) to A
};

inline std::ostream& operator<<(std::ostream& os, const StackTransform& trans) {
  trans.show(os);
  return os;
}

bool apply_op(StackTransform& trans, const AsmOp& op);

/*
 * 
 *   STACK OPERATION OPTIMIZER
 * 
 */

struct Optimizer {
  static constexpr int optimize_depth = 20;
  std::vector<AsmOp> asm_code;
  int start_offset = 0;
  const AsmOp* op_[optimize_depth]{};
  AsmOp oq_[optimize_depth]{};
  int l_{0}, l2_{0}, p_{0}, pb_{0}, q_{0};
  StackTransform tr_[optimize_depth];
  int mode_{0};

  Optimizer(std::vector<AsmOp>&& asm_code, int start_offset, int mode)
    : asm_code(std::move(asm_code)), start_offset(start_offset), mode_(mode) {
    unpack();
  }
  void unpack();
  void apply();
  bool find_at_least(int pb);
  bool find();
  void compute_stack_transforms();
  bool find_const_op(int* op_idx, int cst);
  bool is_push_const(int* i, int* c) const;
  bool rewrite_push_const(int i, int c);
  bool is_const_push_xchgs();
  bool rewrite_const_push_xchgs();
  bool is_const_rot(int* c) const;
  bool rewrite_const_rot(int c);
  bool is_const_pop(int* c, int* i) const;
  bool rewrite_const_pop(int c, int i);
  bool rewrite(int p, AsmOp&& new_op);
  bool rewrite(int p, AsmOp&& new_op1, AsmOp&& new_op2);
  bool rewrite(int p, AsmOp&& new_op1, AsmOp&& new_op2, AsmOp&& new_op3);
  bool rewrite(AsmOp&& new_op) {
    return rewrite(p_, std::move(new_op));
  }
  bool rewrite(AsmOp&& new_op1, AsmOp&& new_op2) {
    return rewrite(p_, std::move(new_op1), std::move(new_op2));
  }
  bool rewrite(AsmOp&& new_op1, AsmOp&& new_op2, AsmOp&& new_op3) {
    return rewrite(p_, std::move(new_op1), std::move(new_op2), std::move(new_op3));
  }
  bool rewrite_nop();
  bool is_pred(const std::function<bool(const StackTransform&)>& pred, int min_p = 2);
  bool is_same_as(const StackTransform& trans, int min_p = 2);
  bool is_rot();
  bool is_rotrev();
  bool is_tuck();
  bool is_2dup();
  bool is_2drop();
  bool is_2swap();
  bool is_2over();
  bool is_xchg(int* i, int* j);
  bool is_xchg_xchg(int* i, int* j, int* k, int* l);
  bool is_push(int* i);
  bool is_pop(int* i);
  bool is_pop_pop(int* i, int* j);
  bool is_nop();
  bool is_push_rot(int* i);
  bool is_push_rotrev(int* i);
  bool is_push_xchg(int* i, int* j, int* k);
  bool is_xchg2(int* i, int* j);
  bool is_xcpu(int* i, int* j);
  bool is_puxc(int* i, int* j);
  bool is_push2(int* i, int* j);
  bool is_xchg3(int* i, int* j, int* k);
  bool is_xc2pu(int* i, int* j, int* k);
  bool is_xcpuxc(int* i, int* j, int* k);
  bool is_xcpu2(int* i, int* j, int* k);
  bool is_puxc2(int* i, int* j, int* k);
  bool is_puxcpu(int* i, int* j, int* k);
  bool is_pu2xc(int* i, int* j, int* k);
  bool is_push3(int* i, int* j, int* k);
  bool is_blkswap(int* i, int* j);
  bool is_blkpush(int* i, int* j);
  bool is_blkdrop(int* i);
  bool is_blkdrop2(int* i, int* j);
  bool is_reverse(int* i, int* j);
  bool is_nip_seq(int* i, int* j);
  bool is_pop_blkdrop(int* i, int* k);
  bool is_2pop_blkdrop(int* i, int* j, int* k);

  bool detect_rewrite_big_THROW();
  bool detect_rewrite_MY_store_int();
  bool detect_rewrite_MY_skip_bits();
  bool detect_rewrite_NEWC_PUSH_STUR();
  bool detect_rewrite_LDxx_DROP();
  bool detect_rewrite_SWAP_symmetric();
  bool detect_rewrite_SWAP_PUSH_STUR();
  bool detect_rewrite_SWAP_STxxxR();
  bool detect_rewrite_BOOLNOT_THROWIF();
  bool detect_rewrite_0EQINT_THROWIF();
  bool detect_rewrite_DICTSETB_DICTSET();
  bool detect_rewrite_DICTGET_NULLSWAPIFNOT_THROWIFNOT();
  bool detect_rewrite_ENDC_CTOS();
  bool detect_rewrite_ENDC_HASHCU();
  bool detect_rewrite_NEWC_BTOS();
  bool detect_rewrite_NEWC_STSLICECONST_BTOS();
  bool detect_rewrite_NEWC_ENDC_CTOS();
  bool detect_rewrite_NEWC_ENDC();
  bool detect_rewrite_emptySlice_ENDS();
  bool detect_rewrite_N_TUPLE_N_UNTUPLE();
  bool detect_rewrite_PUSHREF_CTOS();
  bool detect_rewrite_xxx_NOT();
  bool replace_BOOLNOT_to_NOT();
};

std::vector<AsmOp> optimize_asm_code(std::vector<AsmOp>&& asm_code);

typedef int const_idx_t;

struct StackItemInfo {
  var_idx_t var_idx = 0;
  const_idx_t const_idx = -1;

  StackItemInfo() = default;
  StackItemInfo(var_idx_t var_idx, const_idx_t const_idx)
    : var_idx(var_idx), const_idx(const_idx) {}
  bool operator==(const StackItemInfo& rhs) const = default;
};
typedef std::vector<var_idx_t> StackLayoutVars;

struct Stack {
  std::vector<StackItemInfo> s;
  AsmOpList& o;
  std::vector<td::RefInt256>& unique_constants;
  const std::vector<TmpVar>& named_vars;
  enum { _DisableOut = 128, _InsideLet = 256, _InlineFunc = 512, _InlineAny = 1024, _NeedRetAlt = 2048 };
  int mode;
  Stack(AsmOpList& _o, std::vector<td::RefInt256>& constants, const std::vector<TmpVar>& named_vars, int _mode)
    : o(_o), unique_constants(constants), named_vars(named_vars), mode(_mode) {
  }
  int depth() const {
    return (int)s.size();
  }
  StackItemInfo& at(int i) {
    validate(i);
    return s[depth() - i - 1];
  }
  StackItemInfo get(int i) const {
    validate(i);
    return s[depth() - i - 1];
  }
  bool output_enabled() const {
    return !(mode & _DisableOut);
  }
  void disable_output() {
    mode |= _DisableOut;
  }
  StackLayoutVars vars() const;
  int find(var_idx_t var_idx, int from = 0) const;
  int find(var_idx_t var_idx, int from, int to) const;
  int find_const(const_idx_t const_idx, int from = 0) const;
  int find_outside(var_idx_t var, int from, int to) const;
  const_idx_t register_const(td::RefInt256 new_const);
  void forget_const();
  void validate(int i) const {
    if (i > 255) {
      throw Fatal("Too deep stack");
    }
    tolk_assert(i >= 0 && i < depth() && "invalid stack reference");
  }
  void issue_pop(AnyV origin, int i);
  void issue_push(AnyV origin, int i);
  void issue_xchg(AnyV origin, int i, int j);
  void drop_vars_except(AnyV origin, const VarDescrList& var_info);
  void push_new_var(var_idx_t var_idx);
  void push_new_const(var_idx_t var_idx, const_idx_t const_idx);
  void assign_var(var_idx_t new_idx, var_idx_t old_idx);
  void do_copy_var(AnyV origin, var_idx_t new_idx, var_idx_t old_idx);
  void enforce_state(AnyV origin, const StackLayoutVars& req_stack);
  void rearrange_top(AnyV origin, const StackLayoutVars& top, std::vector<bool> last);
  void rearrange_top(AnyV origin, var_idx_t top_var_idx, bool last);
  void merge_const(const Stack& req_stack);
  void merge_state(AnyV origin, const Stack& req_stack);
  void save_stack_comment(AnyV origin) const;
  void apply_wrappers_if_retalt(AnyV origin, int callxargs_count);
};

/*
 *
 *   SPECIFIC SYMBOL VALUES,
 *   BUILT-IN FUNCTIONS AND OPERATIONS
 * 
 */

struct FunctionBodyBuiltinAsmOp {
  using CompileToAsmOpImpl = AsmOp(std::vector<VarDescr>&, std::vector<VarDescr>&, AnyV origin);
  
  std::function<CompileToAsmOpImpl> simple_compile;

  explicit FunctionBodyBuiltinAsmOp(std::function<CompileToAsmOpImpl> compile)
    : simple_compile(std::move(compile)) {}

  void compile(AsmOpList& dest, std::vector<VarDescr>& out, std::vector<VarDescr>& in, AnyV origin) const;
};

struct FunctionBodyBuiltinGenerateOps {
  using GenerateOpsImpl = std::vector<var_idx_t>(FunctionPtr, CodeBlob&, AnyV origin, const std::vector<std::vector<var_idx_t>>&);

  std::function<GenerateOpsImpl> generate_ops;
  
  explicit FunctionBodyBuiltinGenerateOps(std::function<GenerateOpsImpl> generate_ops)
    : generate_ops(std::move(generate_ops)) {}
};

struct FunctionBodyAsm {
  std::vector<AsmOp> ops;

  void set_code(std::vector<AsmOp>&& code);
  void compile(AsmOpList& dest, AnyV origin) const;
};

struct LazyVariableLoadedState;

// LazyVarRefAtCodegen is a mutable state of a variable assigned by `lazy` operator:
// > var p = lazy Point.fromSlice(s)
// When inlining a method `p.getX()`, `self` also becomes lazy, pointing to the same state.
struct LazyVarRefAtCodegen {
  LocalVarPtr var_ref;
  const LazyVariableLoadedState* var_state;

  LazyVarRefAtCodegen(LocalVarPtr var_ref, const LazyVariableLoadedState* var_state)
    : var_ref(var_ref), var_state(var_state) {}
};

struct CodeBlob {
  int var_cnt, in_var_cnt;
  FunctionPtr fun_ref;
  std::vector<TmpVar> vars;
  std::vector<LazyVarRefAtCodegen> lazy_variables;
  std::vector<var_idx_t>* inline_rvect_out = nullptr;
  bool inlining_before_immediate_return = false;
  OpList ops;
  OpList* cur_ops;
  std::stack<OpList*> cur_ops_stack;
  bool require_callxargs = false;
  explicit CodeBlob(FunctionPtr fun_ref)
    : var_cnt(0), in_var_cnt(0), fun_ref(fun_ref), cur_ops(&ops) {
  }
  void add_call(AnyV origin, std::vector<var_idx_t> ret, std::vector<var_idx_t> args, FunctionPtr called_f,
                bool arg_order_already_equals_asm = false) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_Call, std::move(ret)));
    op.right = std::move(args);
    op.f_sym = called_f;
    if (!called_f->is_marked_as_pure()) op.set_impure_flag();
    if (arg_order_already_equals_asm) op.set_arg_order_already_equals_asm_flag();
  }
  void add_indirect_invoke(AnyV origin, std::vector<var_idx_t> ret, std::vector<var_idx_t> args) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_CallInd, std::move(ret)));
    op.right = std::move(args);
    op.set_impure_flag();
  }
  void add_let(AnyV origin, std::vector<var_idx_t> dst, std::vector<var_idx_t> src) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_Let, std::move(dst)));
    op.right = std::move(src);
  }
  void add_int_const(AnyV origin, std::vector<var_idx_t> dst, td::RefInt256 value) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_IntConst, std::move(dst)));
    op.int_const = std::move(value);
  }
  void add_slice_const(AnyV origin, std::vector<var_idx_t> dst, std::string hex) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_SliceConst, std::move(dst)));
    op.str_const = std::move(hex);
  }
  void add_string_const(AnyV origin, std::vector<var_idx_t> dst, std::string value) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_SnakeStringConst, std::move(dst)));
    op.str_const = std::move(value);
  }
  void add_read_glob_var(AnyV origin, std::vector<var_idx_t> dst, GlobalVarPtr g) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_GlobVar, std::move(dst)));
    op.g_sym = g;
  }
  void add_read_glob_var(AnyV origin, std::vector<var_idx_t> dst, FunctionPtr f) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_GlobVar, std::move(dst)));
    op.f_sym = f;
  }
  void add_set_glob_var(AnyV origin, std::vector<var_idx_t> src, GlobalVarPtr g) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_SetGlob));
    op.right = std::move(src);
    op.g_sym = g;
    op.set_impure_flag();
  }
  void add_import_fun_params(AnyV origin, std::vector<var_idx_t> ir_params) {
    cur_ops->push_back(std::make_unique<Op>(origin, Op::_Import, std::move(ir_params)));
  }
  void add_return(AnyV origin, std::vector<var_idx_t> ir_return = {}) {
    cur_ops->push_back(std::make_unique<Op>(origin, Op::_Return, std::move(ir_return)));
  }
  void add_to_tuple(AnyV origin, std::vector<var_idx_t> dst, std::vector<var_idx_t> src) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_Tuple, std::move(dst)));
    op.right = std::move(src);
  }
  void add_un_tuple(AnyV origin, std::vector<var_idx_t> dst, std::vector<var_idx_t> src) {
    Op& op = cur_ops->push_back(std::make_unique<Op>(origin, Op::_UnTuple, std::move(dst)));
    op.right = std::move(src);
  }
  Op& add_if_else(AnyV origin, std::vector<var_idx_t> cond) {
    return cur_ops->push_back(std::make_unique<Op>(origin, Op::_If, std::move(cond)));
  }
  Op& add_while_loop(AnyV origin) {
    return cur_ops->push_back(std::make_unique<Op>(origin, Op::_While));
  }
  Op& add_until_loop(AnyV origin) {
    return cur_ops->push_back(std::make_unique<Op>(origin, Op::_Until));
  }
  Op& add_repeat_loop(AnyV origin, std::vector<var_idx_t> count) {
    return cur_ops->push_back(std::make_unique<Op>(origin, Op::_Repeat, std::move(count)));
  }
  Op& add_try_catch(AnyV origin) {
    return cur_ops->push_back(std::make_unique<Op>(origin, Op::_TryCatch));
  }
  std::vector<var_idx_t> create_var(TypePtr var_type, AnyV origin, std::string name);
  std::vector<var_idx_t> create_tmp_var(TypePtr var_type, AnyV origin, const char* purpose) {
    std::vector ir_idx = create_var(var_type, origin, {});
#ifdef TOLK_DEBUG
    for (var_idx_t v : ir_idx) {
      vars[v].purpose = purpose;
    }
#endif
    return ir_idx;
  }
  var_idx_t create_int(AnyV origin, int64_t value, const char* purpose);
  bool compute_used_code_vars();
  void print(std::ostream& os, int flags = 0) const;
  void push_set_cur(OpList& new_cur_ops) {
    cur_ops_stack.push(cur_ops);
    cur_ops = &new_cur_ops;
  }
  void close_blk(AnyV origin) {
    cur_ops->push_back(OpList::make_terminal_nop(origin));
  }
  void pop_cur() {
    cur_ops = cur_ops_stack.top();
    cur_ops_stack.pop();
  }
  void close_pop_cur(AnyV origin) {
    close_blk(origin);
    pop_cur();
  }
  const LazyVariableLoadedState* get_lazy_variable(LocalVarPtr var_ref) const;
  const LazyVariableLoadedState* get_lazy_variable(AnyExprV v) const;
  void prune_unreachable_code();
  void fwd_analyze();
  void mark_noreturn();

  std::vector<AsmOp> generate_asm_code(int mode) const;

  static std::string fift_name(FunctionPtr fun_ref);
  static std::string fift_name(GlobalVarPtr var_ref);
};

}  // namespace tolk
