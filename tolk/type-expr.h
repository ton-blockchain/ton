#pragma once

#include <vector>
#include <iostream>

namespace tolk {

struct TypeExpr {
  enum Kind { te_Unknown, te_Var, te_Indirect, te_Atomic, te_Tensor, te_Tuple, te_Map, te_ForAll };
  enum AtomicType { _Int, _Cell, _Slice, _Builder, _Continutaion, _Tuple };
  Kind constr;
  int value;
  int minw, maxw;
  static constexpr int w_inf = 1023;
  std::vector<TypeExpr*> args;
  bool was_forall_var = false;

  explicit TypeExpr(Kind _constr, int _val = 0) : constr(_constr), value(_val), minw(0), maxw(w_inf) {
  }
  TypeExpr(Kind _constr, int _val, int width) : constr(_constr), value(_val), minw(width), maxw(width) {
  }
  TypeExpr(Kind _constr, std::vector<TypeExpr*> list)
      : constr(_constr), value((int)list.size()), args(std::move(list)) {
    compute_width();
  }
  TypeExpr(Kind _constr, std::initializer_list<TypeExpr*> list)
      : constr(_constr), value((int)list.size()), args(std::move(list)) {
    compute_width();
  }
  TypeExpr(Kind _constr, TypeExpr* elem0) : constr(_constr), value(1), args{elem0} {
    compute_width();
  }
  TypeExpr(Kind _constr, TypeExpr* elem0, std::vector<TypeExpr*> list)
      : constr(_constr), value((int)list.size() + 1), args{elem0} {
    args.insert(args.end(), list.begin(), list.end());
    compute_width();
  }
  TypeExpr(Kind _constr, TypeExpr* elem0, std::initializer_list<TypeExpr*> list)
      : constr(_constr), value((int)list.size() + 1), args{elem0} {
    args.insert(args.end(), list.begin(), list.end());
    compute_width();
  }

  bool is_atomic() const {
    return constr == te_Atomic;
  }
  bool is_atomic(int v) const {
    return constr == te_Atomic && value == v;
  }
  bool is_int() const {
    return is_atomic(_Int);
  }
  bool is_var() const {
    return constr == te_Var;
  }
  bool is_map() const {
    return constr == te_Map;
  }
  bool is_tuple() const {
    return constr == te_Tuple;
  }
  bool has_fixed_width() const {
    return minw == maxw;
  }
  int get_width() const {
    return has_fixed_width() ? minw : -1;
  }
  void compute_width();
  bool recompute_width();
  void show_width(std::ostream& os);
  std::ostream& print(std::ostream& os, int prio = 0) const;
  void replace_with(TypeExpr* te2);
  int extract_components(std::vector<TypeExpr*>& comp_list);
  bool equals_to(const TypeExpr* rhs) const;
  bool has_unknown_inside() const;
  static int holes, type_vars;
  static TypeExpr* new_hole() {
    return new TypeExpr{te_Unknown, ++holes};
  }
  static TypeExpr* new_hole(int width) {
    return new TypeExpr{te_Unknown, ++holes, width};
  }
  static TypeExpr* new_unit() {
    return new TypeExpr{te_Tensor, 0, 0};
  }
  static TypeExpr* new_atomic(int value) {
    return new TypeExpr{te_Atomic, value, 1};
  }
  static TypeExpr* new_map(TypeExpr* from, TypeExpr* to);
  static TypeExpr* new_func() {
    return new_map(new_hole(), new_hole());
  }
  static TypeExpr* new_tensor(std::vector<TypeExpr*> list, bool red = true) {
    return red && list.size() == 1 ? list[0] : new TypeExpr{te_Tensor, std::move(list)};
  }
  static TypeExpr* new_tensor(std::initializer_list<TypeExpr*> list) {
    return new TypeExpr{te_Tensor, std::move(list)};
  }
  static TypeExpr* new_tensor(TypeExpr* te1, TypeExpr* te2) {
    return new_tensor({te1, te2});
  }
  static TypeExpr* new_tensor(TypeExpr* te1, TypeExpr* te2, TypeExpr* te3) {
    return new_tensor({te1, te2, te3});
  }
  static TypeExpr* new_tuple(TypeExpr* arg0) {
    return new TypeExpr{te_Tuple, arg0};
  }
  static TypeExpr* new_tuple(std::vector<TypeExpr*> list, bool red = false) {
    return new_tuple(new_tensor(std::move(list), red));
  }
  static TypeExpr* new_tuple(std::initializer_list<TypeExpr*> list) {
    return new_tuple(new_tensor(list));
  }
  static TypeExpr* new_var() {
    return new TypeExpr{te_Var, --type_vars, 1};
  }
  static TypeExpr* new_var(int idx) {
    return new TypeExpr{te_Var, idx, 1};
  }
  static TypeExpr* new_forall(std::vector<TypeExpr*> list, TypeExpr* body) {
    return new TypeExpr{te_ForAll, body, std::move(list)};
  }

  static bool remove_indirect(TypeExpr*& te, TypeExpr* forbidden = nullptr);
  static std::vector<TypeExpr*> remove_forall(TypeExpr*& te);
  static bool remove_forall_in(TypeExpr*& te, TypeExpr* te2, const std::vector<TypeExpr*>& new_vars);
};

std::ostream& operator<<(std::ostream& os, TypeExpr* type_expr);

} // namespace tolk
