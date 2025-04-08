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

#include "platform-utils.h"
#include "src-file.h"
#include "symtable.h"
#include "crypto/common/refint.h"
#include "td/utils/Status.h"
#include <functional>
#include <vector>
#include <string>
#include <stack>
#include <iostream>

#define tolk_assert(expr) if(UNLIKELY(!(expr))) on_assertion_failed(#expr, __FILE__, __LINE__);

namespace tolk {

GNU_ATTRIBUTE_COLD GNU_ATTRIBUTE_NORETURN
void on_assertion_failed(const char *description, const char *file_name, int line_number);

/*
 * 
 *   ABSTRACT CODE
 * 
 */

typedef int var_idx_t;
typedef int const_idx_t;

struct TmpVar {
  var_idx_t ir_idx;   // every var in IR represents 1 stack slot
  TypePtr v_type;     // get_width_on_stack() is 1
  std::string name;   // "x" for vars originated from user sources; "x.0" for tensor components; empty for implicitly created tmp vars
  SrcLocation loc;    // location of var declaration in sources or where a tmp var was originated
#ifdef TOLK_DEBUG
  const char* desc = nullptr; // "origin" of tmp var, for debug output like `'15 (binary-op) '16 (glob-var)`
#endif

  TmpVar(var_idx_t ir_idx, TypePtr v_type, std::string name, SrcLocation loc)
    : ir_idx(ir_idx)
    , v_type(v_type)
    , name(std::move(name))
    , loc(loc) {
  }

  void show_as_stack_comment(std::ostream& os) const;
  void show(std::ostream& os) const;
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
    _Finite = 4096,
    _Nan = 8192,
    _Even = 16384,
    _Odd = 32768,
  };
  static constexpr int ConstZero  = _Const | _Int | _Zero | _Pos | _Neg | _Finite | _Even;
  static constexpr int ConstOne   = _Const | _Int | _NonZero | _Pos | _Finite | _Odd;
  static constexpr int ConstTrue  = _Const | _Int | _NonZero | _Neg | _Finite | _Odd;
  static constexpr int ValBit     = _Int | _Pos | _Finite;
  static constexpr int ValBool    = _Int | _Neg | _Finite;
  static constexpr int FiniteInt  = _Int | _Finite;
  static constexpr int FiniteUInt = _Int | _Finite | _Pos;
  int val;
  td::RefInt256 int_const;
  std::string str_const;

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
  void set_const(std::string value);
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

template <typename T>
class ListIterator {
  T* ptr;

 public:
  ListIterator() : ptr(nullptr) {
  }
  explicit ListIterator(T* _ptr) : ptr(_ptr) {
  }
  ListIterator& operator++() {
    ptr = ptr->next.get();
    return *this;
  }
  ListIterator operator++(int) {
    T* z = ptr;
    ptr = ptr->next.get();
    return ListIterator{z};
  }
  T& operator*() const {
    return *ptr;
  }
  T* operator->() const {
    return ptr;
  }
  bool operator==(const ListIterator& y) const {
    return ptr == y.ptr;
  }
  bool operator!=(const ListIterator& y) const {
    return ptr != y.ptr;
  }
};

struct Stack;

struct Op {
  enum OpKind {
    _Undef,
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
  };
  OpKind cl;
  enum { _Disabled = 1, _NoReturn = 4, _Impure = 24 };
  int flags;
  std::unique_ptr<Op> next;
  FunctionPtr f_sym = nullptr;
  GlobalVarPtr g_sym = nullptr;
  SrcLocation where;
  VarDescrList var_info;
  std::vector<VarDescr> args;
  std::vector<var_idx_t> left, right;
  std::unique_ptr<Op> block0, block1;
  td::RefInt256 int_const;
  std::string str_const;
  Op(SrcLocation _where = {}, OpKind _cl = _Undef) : cl(_cl), flags(0), f_sym(nullptr), where(_where) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(_left) {
  }
  Op(SrcLocation _where, OpKind _cl, std::vector<var_idx_t>&& _left)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(std::move(_left)) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left, td::RefInt256 _const)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(_left), int_const(_const) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left, std::string _const)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(_left), str_const(_const) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left, const std::vector<var_idx_t>& _right)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(_left), right(_right) {
  }
  Op(SrcLocation _where, OpKind _cl, std::vector<var_idx_t>&& _left, std::vector<var_idx_t>&& _right)
      : cl(_cl), flags(0), f_sym(nullptr), where(_where), left(std::move(_left)), right(std::move(_right)) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left, const std::vector<var_idx_t>& _right,
     FunctionPtr _fun)
      : cl(_cl), flags(0), f_sym(_fun), where(_where), left(_left), right(_right) {
  }
  Op(SrcLocation _where, OpKind _cl, std::vector<var_idx_t>&& _left, std::vector<var_idx_t>&& _right,
     FunctionPtr _fun)
      : cl(_cl), flags(0), f_sym(_fun), where(_where), left(std::move(_left)), right(std::move(_right)) {
  }
  Op(SrcLocation _where, OpKind _cl, const std::vector<var_idx_t>& _left, const std::vector<var_idx_t>& _right,
     GlobalVarPtr _gvar)
      : cl(_cl), flags(0), g_sym(_gvar), where(_where), left(_left), right(_right) {
  }
  Op(SrcLocation _where, OpKind _cl, std::vector<var_idx_t>&& _left, std::vector<var_idx_t>&& _right,
     GlobalVarPtr _gvar)
      : cl(_cl), flags(0), g_sym(_gvar), where(_where), left(std::move(_left)), right(std::move(_right)) {
  }

  bool disabled() const { return flags & _Disabled; }
  void set_disabled() { flags |= _Disabled; }
  void set_disabled(bool flag);

  bool noreturn() const { return flags & _NoReturn; }
  bool set_noreturn() { flags |= _NoReturn; return true; }
  bool set_noreturn(bool flag);

  bool impure() const { return flags & _Impure; }
  void set_impure_flag();

  void show(std::ostream& os, const std::vector<TmpVar>& vars, std::string pfx = "", int mode = 0) const;
  void show_var_list(std::ostream& os, const std::vector<var_idx_t>& idx_list, const std::vector<TmpVar>& vars) const;
  void show_var_list(std::ostream& os, const std::vector<VarDescr>& list, const std::vector<TmpVar>& vars) const;
  static void show_block(std::ostream& os, const Op* block, const std::vector<TmpVar>& vars, std::string pfx = "",
                         int mode = 0);
  bool compute_used_vars(const CodeBlob& code, bool edit);
  bool std_compute_used_vars(bool disabled = false);
  bool set_var_info(const VarDescrList& new_var_info);
  bool set_var_info(VarDescrList&& new_var_info);
  bool set_var_info_except(const VarDescrList& new_var_info, const std::vector<var_idx_t>& var_list);
  bool set_var_info_except(VarDescrList&& new_var_info, const std::vector<var_idx_t>& var_list);
  void prepare_args(VarDescrList values);
  VarDescrList fwd_analyze(VarDescrList values);
  bool mark_noreturn();
  bool is_empty() const {
    return cl == _Nop && !next;
  }
  bool generate_code_step(Stack& stack);
  void generate_code_all(Stack& stack);
  Op& last() {
    return next ? next->last() : *this;
  }
  const Op& last() const {
    return next ? next->last() : *this;
  }
};

inline ListIterator<Op> begin(const std::unique_ptr<Op>& op_list) {
  return ListIterator<Op>{op_list.get()};
}

inline ListIterator<Op> end(const std::unique_ptr<Op>& op_list) {
  return ListIterator<Op>{};
}

inline ListIterator<const Op> begin(const Op* op_list) {
  return ListIterator<const Op>{op_list};
}

inline ListIterator<const Op> end(const Op* op_list) {
  return ListIterator<const Op>{};
}

struct AsmOpList;

struct FunctionBodyCode {
  CodeBlob* code = nullptr;
  void set_code(CodeBlob* code);
};

/*
 * 
 *   GENERATE CODE
 * 
 */

typedef std::vector<var_idx_t> StackLayout;
typedef std::pair<var_idx_t, const_idx_t> var_const_idx_t;
typedef std::vector<var_const_idx_t> StackLayoutExt;
constexpr const_idx_t not_const = -1;
using Const = td::RefInt256;

struct AsmOp {
  enum Type { a_none, a_xchg, a_push, a_pop, a_const, a_custom, a_magic };
  Type t{a_none};
  int indent{0};
  int a, b;
  bool gconst{false};
  std::string op;
  struct SReg {
    int idx;
    SReg(int _idx) : idx(_idx) {
    }
  };
  AsmOp() = default;
  AsmOp(Type _t) : t(_t) {
  }
  AsmOp(Type _t, std::string _op) : t(_t), op(std::move(_op)) {
  }
  AsmOp(Type _t, int _a) : t(_t), a(_a) {
  }
  AsmOp(Type _t, int _a, std::string _op) : t(_t), a(_a), op(std::move(_op)) {
  }
  AsmOp(Type _t, int _a, int _b) : t(_t), a(_a), b(_b) {
  }
  AsmOp(Type _t, int _a, int _b, std::string _op) : t(_t), a(_a), b(_b), op(std::move(_op)) {
    compute_gconst();
  }
  void out(std::ostream& os) const;
  void out_indent_nl(std::ostream& os, bool no_nl = false) const;
  std::string to_string() const;
  void compute_gconst() {
    gconst = (is_custom() && (op == "PUSHNULL" || op == "NEWC" || op == "NEWB" || op == "TRUE" || op == "FALSE" || op == "NOW"));
  }
  bool is_nop() const {
    return t == a_none && op.empty();
  }
  bool is_comment() const {
    return t == a_none && !op.empty();
  }
  bool is_custom() const {
    return t == a_custom;
  }
  bool is_very_custom() const {
    return is_custom() && a >= 255;
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
  static AsmOp Nop() {
    return AsmOp(a_none);
  }
  static AsmOp Xchg(int a, int b = 0) {
    return a == b ? AsmOp(a_none) : (a < b ? AsmOp(a_xchg, a, b) : AsmOp(a_xchg, b, a));
  }
  static AsmOp Push(int a) {
    return AsmOp(a_push, a);
  }
  static AsmOp Pop(int a = 0) {
    return AsmOp(a_pop, a);
  }
  static AsmOp Xchg2(int a, int b) {
    return make_stk2(a, b, "XCHG2", 0);
  }
  static AsmOp XcPu(int a, int b) {
    return make_stk2(a, b, "XCPU", 1);
  }
  static AsmOp PuXc(int a, int b) {
    return make_stk2(a, b, "PUXC", 1);
  }
  static AsmOp Push2(int a, int b) {
    return make_stk2(a, b, "PUSH2", 2);
  }
  static AsmOp Xchg3(int a, int b, int c) {
    return make_stk3(a, b, c, "XCHG3", 0);
  }
  static AsmOp Xc2Pu(int a, int b, int c) {
    return make_stk3(a, b, c, "XC2PU", 1);
  }
  static AsmOp XcPuXc(int a, int b, int c) {
    return make_stk3(a, b, c, "XCPUXC", 1);
  }
  static AsmOp XcPu2(int a, int b, int c) {
    return make_stk3(a, b, c, "XCPU2", 3);
  }
  static AsmOp PuXc2(int a, int b, int c) {
    return make_stk3(a, b, c, "PUXC2", 3);
  }
  static AsmOp PuXcPu(int a, int b, int c) {
    return make_stk3(a, b, c, "PUXCPU", 3);
  }
  static AsmOp Pu2Xc(int a, int b, int c) {
    return make_stk3(a, b, c, "PU2XC", 3);
  }
  static AsmOp Push3(int a, int b, int c) {
    return make_stk3(a, b, c, "PUSH3", 3);
  }
  static AsmOp BlkSwap(int a, int b);
  static AsmOp BlkPush(int a, int b);
  static AsmOp BlkDrop(int a);
  static AsmOp BlkDrop2(int a, int b);
  static AsmOp BlkReverse(int a, int b);
  static AsmOp make_stk2(int a, int b, const char* str, int delta);
  static AsmOp make_stk3(int a, int b, int c, const char* str, int delta);
  static AsmOp IntConst(const td::RefInt256& x);
  static AsmOp BoolConst(bool f);
  static AsmOp Const(std::string push_op) {
    return AsmOp(a_const, 0, 1, std::move(push_op));
  }
  static AsmOp Const(int arg, const std::string& push_op);
  static AsmOp Comment(const std::string& comment) {
    return AsmOp(a_none, std::string{"// "} + comment);
  }
  static AsmOp Custom(const std::string& custom_op) {
    return AsmOp(a_custom, 255, 255, custom_op);
  }
  static AsmOp Parse(const std::string& custom_op);
  static AsmOp Custom(const std::string& custom_op, int args, int retv = 1) {
    return AsmOp(a_custom, args, retv, custom_op);
  }
  static AsmOp Parse(std::string custom_op, int args, int retv = 1);
  static AsmOp Tuple(int a);
  static AsmOp UnTuple(int a);
};

inline std::ostream& operator<<(std::ostream& os, const AsmOp& op) {
  op.out(os);
  return os;
}

std::ostream& operator<<(std::ostream& os, AsmOp::SReg stack_reg);
std::ostream& operator<<(std::ostream& os, TypePtr type_data);

struct AsmOpList {
  std::vector<AsmOp> list_;
  int indent_{0};
  const std::vector<TmpVar>* var_names_{nullptr};
  std::vector<Const> constants_;
  bool retalt_{false};
  bool retalt_inserted_{false};
  void out(std::ostream& os, int mode = 0) const;
  AsmOpList(int indent = 0, const std::vector<TmpVar>* var_names = nullptr) : indent_(indent), var_names_(var_names) {
  }
  template <typename... Args>
  AsmOpList& add(Args&&... args) {
    append(AsmOp(std::forward<Args>(args)...));
    adjust_last();
    return *this;
  }
  bool append(const AsmOp& op) {
    list_.push_back(op);
    adjust_last();
    return true;
  }
  bool append(const std::vector<AsmOp>& ops);
  bool append(std::initializer_list<AsmOp> ops) {
    return append(std::vector<AsmOp>(std::move(ops)));
  }
  AsmOpList& operator<<(const AsmOp& op) {
    return add(op);
  }
  AsmOpList& operator<<(AsmOp&& op) {
    return add(std::move(op));
  }
  AsmOpList& operator<<(std::string str) {
    return add(AsmOp::Type::a_custom, 255, 255, str);
  }
  const_idx_t register_const(Const new_const);
  Const get_const(const_idx_t idx);
  void show_var_ext(std::ostream& os, std::pair<var_idx_t, const_idx_t> idx_pair) const;
  void adjust_last() {
    if (list_.back().is_nop()) {
      list_.pop_back();
    } else {
      list_.back().indent = indent_;
    }
  }
  void indent() {
    ++indent_;
  }
  void undent() {
    --indent_;
  }
  void set_indent(int new_indent) {
    indent_ = new_indent;
  }
  void insert(size_t pos, std::string str) {
    insert(pos, AsmOp(AsmOp::a_custom, 255, 255, str));
  }
  void insert(size_t pos, const AsmOp& op) {
    auto ip = list_.begin() + pos;
    ip = list_.insert(ip, op);
    ip->indent = (ip == list_.begin()) ? indent_ : (ip - 1)->indent;
  }
  void indent_all() {
    for (auto &op : list_) {
      ++op.indent;
    }
  }
};

inline std::ostream& operator<<(std::ostream& os, const AsmOpList& op_list) {
  op_list.out(os);
  return os;
}

struct AsmOpCons {
  std::unique_ptr<AsmOp> car;
  std::unique_ptr<AsmOpCons> cdr;
  AsmOpCons(std::unique_ptr<AsmOp> head, std::unique_ptr<AsmOpCons> tail) : car(std::move(head)), cdr(std::move(tail)) {
  }
  static std::unique_ptr<AsmOpCons> cons(std::unique_ptr<AsmOp> head, std::unique_ptr<AsmOpCons> tail) {
    return std::make_unique<AsmOpCons>(std::move(head), std::move(tail));
  }
};

using AsmOpConsList = std::unique_ptr<AsmOpCons>;

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
  bool operator==(const StackTransform& other) const {
    return dp == other.dp && almost_equal(other);
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
  int operator()(int i) const {
    return get(i);
  }
  class Pos {
    StackTransform& t_;
    int p_;

   public:
    Pos(StackTransform& t, int p) : t_(t), p_(p) {
    }
    Pos& operator=(const Pos& other) = delete;
    operator int() const {
      return t_.get(p_);
    }
    const Pos& operator=(int v) const {
      t_.set(p_, v);
      return *this;
    }
  };
  Pos operator[](int i) {
    return Pos(*this, i);
  }
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
  static StackTransform Push(int i);
  static StackTransform Pop(int i);

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
  AsmOpConsList code_;
  int l_{0}, l2_{0}, p_, pb_, q_, indent_;
  bool debug_{false};
  std::unique_ptr<AsmOp> op_[optimize_depth], oq_[optimize_depth];
  AsmOpCons* op_cons_[optimize_depth];
  int offs_[optimize_depth];
  StackTransform tr_[optimize_depth];
  int mode_{0};
  Optimizer() {
  }
  Optimizer(bool debug, int mode = 0) : debug_(debug), mode_(mode) {
  }
  Optimizer(AsmOpConsList code, bool debug = false, int mode = 0) : Optimizer(debug, mode) {
    set_code(std::move(code));
  }
  void set_code(AsmOpConsList code_);
  void unpack();
  void pack();
  void apply();
  bool find_at_least(int pb);
  bool find();
  bool optimize();
  bool compute_stack_transforms();
  bool say(std::string str) const;
  bool show_stack_transforms() const;
  void show_head() const;
  void show_left() const;
  void show_right() const;
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
  AsmOpConsList extract_code();
};

AsmOpConsList optimize_code_head(AsmOpConsList op_list, int mode = 0);
AsmOpConsList optimize_code(AsmOpConsList op_list, int mode);
void optimize_code(AsmOpList& ops);

struct Stack {
  StackLayoutExt s;
  AsmOpList& o;
  enum {
    _StkCmt = 1, _CptStkCmt = 2, _DisableOut = 128, _Shown = 256,
    _InlineFunc = 512, _NeedRetAlt = 1024, _InlineAny = 2048,
    _ModeSave = _InlineFunc | _NeedRetAlt | _InlineAny,
    _Garbage = -0x10000
  };
  int mode;
  Stack(AsmOpList& _o, int _mode = 0) : o(_o), mode(_mode) {
  }
  Stack(AsmOpList& _o, const StackLayoutExt& _s, int _mode = 0) : s(_s), o(_o), mode(_mode) {
  }
  Stack(AsmOpList& _o, StackLayoutExt&& _s, int _mode = 0) : s(std::move(_s)), o(_o), mode(_mode) {
  }
  int depth() const {
    return (int)s.size();
  }
  var_idx_t operator[](int i) const {
    validate(i);
    return s[depth() - i - 1].first;
  }
  var_const_idx_t& at(int i) {
    validate(i);
    return s[depth() - i - 1];
  }
  var_const_idx_t at(int i) const {
    validate(i);
    return s[depth() - i - 1];
  }
  var_const_idx_t get(int i) const {
    return at(i);
  }
  bool output_disabled() const {
    return mode & _DisableOut;
  }
  bool output_enabled() const {
    return !output_disabled();
  }
  void disable_output() {
    mode |= _DisableOut;
  }
  StackLayout vars() const;
  int find(var_idx_t var, int from = 0) const;
  int find(var_idx_t var, int from, int to) const;
  int find_const(const_idx_t cst, int from = 0) const;
  int find_outside(var_idx_t var, int from, int to) const;
  void forget_const();
  void validate(int i) const {
    if (i > 255) {
      throw Fatal{"Too deep stack"};
    }
    tolk_assert(i >= 0 && i < depth() && "invalid stack reference");
  }
  void modified() {
    mode &= ~_Shown;
  }
  void issue_pop(int i);
  void issue_push(int i);
  void issue_xchg(int i, int j);
  int drop_vars_except(const VarDescrList& var_info, int excl_var = 0x80000000);
  void forget_var(var_idx_t idx);
  void push_new_var(var_idx_t idx);
  void push_new_const(var_idx_t idx, const_idx_t cidx);
  void assign_var(var_idx_t new_idx, var_idx_t old_idx);
  void do_copy_var(var_idx_t new_idx, var_idx_t old_idx);
  void enforce_state(const StackLayout& req_stack);
  void rearrange_top(const StackLayout& top, std::vector<bool> last);
  void rearrange_top(var_idx_t top, bool last);
  void merge_const(const Stack& req_stack);
  void merge_state(const Stack& req_stack);
  void show();
  void opt_show() {
    if ((mode & (_StkCmt | _Shown)) == _StkCmt) {
      show();
    }
  }
  bool operator==(const Stack& y) const & {
    return s == y.s;
  }
  void apply_wrappers(int callxargs_count) {
    bool is_inline = mode & _InlineFunc;
    if (o.retalt_inserted_) {
      o.insert(0, "SAMEALTSAVE");
      o.insert(0, "c2 SAVE");
    }
    if (callxargs_count != -1 || (is_inline && o.retalt_)) {
      o.indent_all();
      o.insert(0, "CONT:<{");
      o << "}>";
      if (callxargs_count != -1) {
        if (callxargs_count <= 15) {
          o << AsmOp::Custom(PSTRING() << callxargs_count << " -1 CALLXARGS");
        } else {
          tolk_assert(callxargs_count <= 254);
          o << AsmOp::Custom(PSTRING() << callxargs_count << " PUSHINT -1 PUSHINT CALLXVARARGS");
        }
      } else {
        o << "EXECUTE";
      }
    }
  }
};

/*
 *
 *   SPECIFIC SYMBOL VALUES,
 *   BUILT-IN FUNCTIONS AND OPERATIONS
 * 
 */

typedef std::function<AsmOp(std::vector<VarDescr>&, std::vector<VarDescr>&, SrcLocation)> simple_compile_func_t;

inline simple_compile_func_t make_simple_compile(AsmOp op) {
  return [op](std::vector<VarDescr>& out, std::vector<VarDescr>& in, SrcLocation) -> AsmOp { return op; };
}

struct FunctionBodyBuiltin {
  simple_compile_func_t simple_compile;

  explicit FunctionBodyBuiltin(simple_compile_func_t compile)
    : simple_compile(std::move(compile)) {}

  void compile(AsmOpList& dest, std::vector<VarDescr>& out, std::vector<VarDescr>& in, SrcLocation where) const;
};

struct FunctionBodyAsm {
  std::vector<AsmOp> ops;

  void set_code(std::vector<AsmOp>&& code);
  void compile(AsmOpList& dest) const;
};

struct CodeBlob {
  int var_cnt, in_var_cnt;
  FunctionPtr fun_ref;
  std::string name;
  SrcLocation loc;
  std::vector<TmpVar> vars;
  std::unique_ptr<Op> ops;
  std::unique_ptr<Op>* cur_ops;
#ifdef TOLK_DEBUG
  std::vector<Op*> _vector_of_ops;  // to see it in debugger instead of nested pointers
#endif
  std::stack<std::unique_ptr<Op>*> cur_ops_stack;
  bool require_callxargs = false;
  CodeBlob(std::string name, SrcLocation loc, FunctionPtr fun_ref)
    : var_cnt(0), in_var_cnt(0), fun_ref(fun_ref), name(std::move(name)), loc(loc), cur_ops(&ops) {
  }
  template <typename... Args>
  Op& emplace_back(Args&&... args) {
    Op& res = *(*cur_ops = std::make_unique<Op>(args...));
    cur_ops = &(res.next);
#ifdef TOLK_DEBUG
    _vector_of_ops.push_back(&res);
#endif
    return res;
  }
  std::vector<var_idx_t> create_var(TypePtr var_type, SrcLocation loc, std::string name);
  std::vector<var_idx_t> create_tmp_var(TypePtr var_type, SrcLocation loc, const char* desc) {
    std::vector<var_idx_t> ir_idx = create_var(var_type, loc, {});
#ifdef TOLK_DEBUG
    for (var_idx_t v : ir_idx) {
      vars[v].desc = desc;
    }
#endif
    return ir_idx;
  }
  bool compute_used_code_vars();
  bool compute_used_code_vars(std::unique_ptr<Op>& ops, const VarDescrList& var_info, bool edit) const;
  void print(std::ostream& os, int flags = 0) const;
  void push_set_cur(std::unique_ptr<Op>& new_cur_ops) {
    cur_ops_stack.push(cur_ops);
    cur_ops = &new_cur_ops;
  }
  void close_blk(SrcLocation location) {
    *cur_ops = std::make_unique<Op>(location, Op::_Nop);
  }
  void pop_cur() {
    cur_ops = cur_ops_stack.top();
    cur_ops_stack.pop();
  }
  void close_pop_cur(SrcLocation location) {
    close_blk(location);
    pop_cur();
  }
  void prune_unreachable_code();
  void fwd_analyze();
  void mark_noreturn();
  void generate_code(AsmOpList& out_list, int mode = 0);
  void generate_code(std::ostream& os, int mode = 0, int indent = 0);
};

// defined in builtins.cpp
AsmOp exec_arg_op(std::string op, long long arg);
AsmOp exec_arg_op(std::string op, long long arg, int args, int retv = 1);
AsmOp exec_arg_op(std::string op, td::RefInt256 arg);
AsmOp exec_arg_op(std::string op, td::RefInt256 arg, int args, int retv = 1);
AsmOp exec_arg2_op(std::string op, long long imm1, long long imm2, int args, int retv = 1);
AsmOp push_const(td::RefInt256 x);

void define_builtins();



/*
 *
 *   OUTPUT CODE GENERATOR
 *
 */

int tolk_proceed(const std::string &entrypoint_filename);

}  // namespace tolk


