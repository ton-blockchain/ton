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
#include <iostream>

namespace tolk {

/*
 * 
 *   ASM-OP LIST FUNCTIONS
 * 
 */

int is_pos_pow2(td::RefInt256 x) {
  if (sgn(x) > 0 && !sgn(x & (x - 1))) {
    return x->bit_size(false) - 1;
  } else {
    return -1;
  }
}

int is_neg_pow2(td::RefInt256 x) {
  return sgn(x) < 0 ? is_pos_pow2(-x) : 0;
}

std::ostream& operator<<(std::ostream& os, AsmOp::SReg stack_reg) {
  int i = stack_reg.idx;
  if (i >= 0) {
    if (i < 16) {
      return os << 's' << i;
    } else {
      return os << i << " s()";
    }
  } else if (i >= -2) {
    return os << "s(" << i << ')';
  } else {
    return os << i << " s()";
  }
}

// mirror the above operator<< formatting, but calculate resulting strlen
// used to align comments in Fift output
int AsmOp::SReg::calc_out_strlen() const {
  int i = idx;
  if (i >= 0) {
    if (i < 10) {
      return 2;
    } else if (i < 16) {
      return 3;
    } else {
      return 6;
    }
  } else if (i >= -2) {
    return 5;
  } else {
    return 6;
  }
}


AsmOp AsmOp::Const(SrcLocation loc, int arg, const std::string& push_op) {
  std::ostringstream os;
  os << arg << ' ' << push_op;
  return AsmOp::Const(loc, os.str());
}

AsmOp AsmOp::make_stk2(SrcLocation loc, int a, int b, const char* str, int delta) {
  std::ostringstream os;
  os << SReg(a) << ' ' << SReg(b) << ' ' << str;
  int c = std::max(a, b) + 1;
  return AsmOp::Custom(loc, os.str(), c, c + delta);
}

AsmOp AsmOp::make_stk3(SrcLocation loc, int a, int b, int c, const char* str, int delta) {
  std::ostringstream os;
  os << SReg(a) << ' ' << SReg(b) << ' ' << SReg(c) << ' ' << str;
  int m = std::max(a, std::max(b, c)) + 1;
  return AsmOp::Custom(loc, os.str(), m, m + delta);
}

AsmOp AsmOp::BlkSwap(SrcLocation loc, int a, int b) {
  std::ostringstream os;
  if (a == 1 && b == 1) {
    return AsmOp::Xchg(loc, 0, 1);
  } else if (a == 1) {
    if (b == 2) {
      os << "ROT";
    } else {
      os << b << " ROLL";
    }
  } else if (b == 1) {
    if (a == 2) {
      os << "-ROT";
    } else {
      os << a << " -ROLL";
    }
  } else {
    os << a << " " << b << " BLKSWAP";
  }
  return AsmOp::Custom(loc, os.str(), a + b, a + b);
}

AsmOp AsmOp::BlkPush(SrcLocation loc, int a, int b) {
  std::ostringstream os;
  if (a == 1) {
    return AsmOp::Push(loc, b);
  } else if (a == 2 && b == 1) {
    os << "2DUP";
  } else {
    os << a << " " << b << " BLKPUSH";
  }
  return AsmOp::Custom(loc, os.str(), b + 1, a + b + 1);
}

AsmOp AsmOp::BlkDrop(SrcLocation loc, int a) {
  std::ostringstream os;
  if (a == 1) {
    return AsmOp::Pop(loc, 0);
  } else if (a == 2) {
    os << "2DROP";
  } else {
    os << a << " BLKDROP";
  }
  return AsmOp::Custom(loc, os.str(), a, 0);
}

AsmOp AsmOp::BlkDrop2(SrcLocation loc, int a, int b) {
  if (!b) {
    return BlkDrop(loc, a);
  }
  std::ostringstream os;
  os << a << " " << b << " BLKDROP2";
  return AsmOp::Custom(loc, os.str(), a + b, b);
}

AsmOp AsmOp::BlkReverse(SrcLocation loc, int a, int b) {
  std::ostringstream os;
  os << a << " " << b << " REVERSE";
  return AsmOp::Custom(loc, os.str(), a + b, a + b);
}

AsmOp AsmOp::Tuple(SrcLocation loc, int a) {
  switch (a) {
    case 1:
      return AsmOp::Custom(loc, "SINGLE", 1, 1);
    case 2:
      return AsmOp::Custom(loc, "PAIR", 2, 1);
    case 3:
      return AsmOp::Custom(loc, "TRIPLE", 3, 1);
  }
  std::ostringstream os;
  os << a << " TUPLE";
  return AsmOp::Custom(loc, os.str(), a, 1);
}

AsmOp AsmOp::UnTuple(SrcLocation loc, int a) {
  switch (a) {
    case 1:
      return AsmOp::Custom(loc, "UNSINGLE", 1, 1);
    case 2:
      return AsmOp::Custom(loc, "UNPAIR", 1, 2);
    case 3:
      return AsmOp::Custom(loc, "UNTRIPLE", 1, 3);
  }
  std::ostringstream os;
  os << a << " UNTUPLE";
  return AsmOp::Custom(loc, os.str(), 1, a);
}

AsmOp AsmOp::IntConst(SrcLocation loc, const td::RefInt256& x) {
  if (x->signed_fits_bits(8)) {
    return AsmOp::Const(loc, dec_string(x) + " PUSHINT");
  }
  if (!x->is_valid()) {
    return AsmOp::Const(loc, "PUSHNAN");
  }
  int k = is_pos_pow2(x);
  if (k >= 0) {
    return AsmOp::Const(loc, k, "PUSHPOW2");
  }
  k = is_pos_pow2(x + 1);
  if (k >= 0) {
    return AsmOp::Const(loc, k, "PUSHPOW2DEC");
  }
  k = is_pos_pow2(-x);
  if (k >= 0) {
    return AsmOp::Const(loc, k, "PUSHNEGPOW2");
  }
  if (!x->mod_pow2_short(23)) {
    return AsmOp::Const(loc, dec_string(x) + " PUSHINTX");
  }
  return AsmOp::Const(loc, dec_string(x) + " PUSHINT");
}

AsmOp AsmOp::BoolConst(SrcLocation loc, bool f) {
  return AsmOp::Const(loc, f ? "TRUE" : "FALSE");
}

AsmOp AsmOp::Parse(SrcLocation loc, const std::string& custom_op) {
  if (custom_op == "NOP") {
    return AsmOp::Nop(loc);
  } else if (custom_op == "SWAP") {
    return AsmOp::Xchg(loc, 1);
  } else if (custom_op == "DROP") {
    return AsmOp::Pop(loc, 0);
  } else if (custom_op == "NIP") {
    return AsmOp::Pop(loc, 1);
  } else if (custom_op == "DUP") {
    return AsmOp::Push(loc, 0);
  } else if (custom_op == "OVER") {
    return AsmOp::Push(loc, 1);
  } else {
    return AsmOp::Custom(loc, custom_op);
  }
}

AsmOp AsmOp::Parse(SrcLocation loc, std::string custom_op, int args, int retv) {
  auto res = Parse(loc, custom_op);
  if (res.is_custom()) {
    res.a = args;
    res.b = retv;
  }
  return res;
}

int AsmOp::out(std::ostream& os) const {
  if (!op.empty()) {
    os << op;
    return static_cast<int>(op.size());   // return strlen to align a comment at the right
  }
  switch (t) {
    case a_nop:
    case a_comment:
      return 0;
    case a_xchg:
      if (!a && !(b & -2)) {
        os << (b ? "SWAP" : "NOP");
        return b ? 4 : 3;
      }
      os << SReg(a) << ' ' << SReg(b) << " XCHG";
      return SReg(a).calc_out_strlen() + 1 + SReg(b).calc_out_strlen() + 5;
    case a_push:
      if (!(a & -2)) {
        os << (a ? "OVER" : "DUP");
        return a ? 4 : 3;
      }
      os << SReg(a) << " PUSH";
      return SReg(a).calc_out_strlen() + 5;
    case a_pop:
      if (!(a & -2)) {
        os << (a ? "NIP" : "DROP");
        return a ? 3 : 4;
      }
      os << SReg(a) << " POP";
      return SReg(a).calc_out_strlen() + 4;
    default:
      throw Fatal("unknown assembler operation");
  }
}

int AsmOp::out_indented(std::ostream& os, bool print_src_line_above) const {
  static int last_line_no = -1;
  if (loc.is_defined() && print_src_line_above) {
    loc.show_line_to_fif_output(os, indent, &last_line_no);
  }
  for (int i = 0; i < indent * 2; i++) {
    os << ' ';
  }
  return out(os) + indent * 2;
}

std::string AsmOp::to_string() const {
  if (!op.empty()) {
    return op;
  } else {
    std::ostringstream os;
    out(os);
    return os.str();
  }
}

const_idx_t AsmOpList::register_const(td::RefInt256 new_const) {
  if (new_const.is_null()) {
    return not_const;
  }
  unsigned idx;
  for (idx = 0; idx < constants_.size(); idx++) {
    if (!td::cmp(new_const, constants_[idx])) {
      return idx;
    }
  }
  constants_.push_back(std::move(new_const));
  return (const_idx_t)idx;
}

td::RefInt256 AsmOpList::get_const(const_idx_t idx) {
  if ((unsigned)idx < constants_.size()) {
    return constants_[idx];
  } else {
    return {};
  }
}

void AsmOpList::show_var_ext(std::ostream& os, std::pair<var_idx_t, const_idx_t> idx_pair) const {
  var_idx_t i = idx_pair.first;
  const_idx_t j = idx_pair.second;
  if (!var_names_ || (unsigned)i >= var_names_->size()) {
    os << '\'' << i;
  } else {
    var_names_->at(i).show_as_stack_comment(os);
  }
  if ((unsigned)j < constants_.size() && constants_[j].not_null()) {
    os << '=' << constants_[j];
  }
}

void AsmOpList::out(std::ostream& os, int mode) const {
  std::size_t n = list_.size();
  for (std::size_t i = 0; i < n; i++) {
    const AsmOp& op = list_[i];
    if (!op.is_comment() && i + 1 < n && list_[i + 1].is_comment()) {
      int len = op.out_indented(os, mode & Stack::_LineComments);
      while (len < 28) {    // align stack comments at the right
        os << ' ';
        len++;
      }
      os << '\t';
      do {
        i++;
      } while (i + 1 < n && list_[i + 1].is_comment());
      list_[i].out(os);
      os << std::endl;
    } else if (op.is_comment()) {
      op.out(os);
      os << std::endl;
    } else {
      op.out_indented(os, mode & Stack::_LineComments);
      os << std::endl;
    }
  }
}

bool apply_op(StackTransform& trans, const AsmOp& op) {
  if (!trans.is_valid()) {
    return false;
  }
  switch (op.t) {
    case AsmOp::a_nop:
      return true;
    case AsmOp::a_xchg:
      return trans.apply_xchg(op.a, op.b, true);
    case AsmOp::a_push:
      return trans.apply_push(op.a);
    case AsmOp::a_pop:
      return trans.apply_pop(op.a);
    case AsmOp::a_const:
      return !op.a && op.b == 1 && trans.apply_push_newconst();
    case AsmOp::a_custom:
      return op.is_gconst() && trans.apply_push_newconst();
    default:
      return false;
  }
}

}  // namespace tolk
