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
#include "symtable.h"
#include <sstream>
#include <cassert>

namespace tolk {

/*
 *
 *   SYMBOL VALUES (DECLARED)
 *
 */

int scope_level;

SymTable symbols;

SymDef* sym_def[symbols.SIZE_PRIME + 1];
SymDef* global_sym_def[symbols.SIZE_PRIME + 1];
std::vector<std::pair<int, SymDef>> symbol_stack;
std::vector<SrcLocation> scope_opened_at;

Symbol::Symbol(std::string str, sym_idx_t idx) : str(std::move(str)), idx(idx) {
  subclass = this->str[0] == '.'   ? SymbolSubclass::dot_identifier
             : this->str[0] == '~' ? SymbolSubclass::tilde_identifier
                                   : SymbolSubclass::undef;
}

std::string Symbol::unknown_symbol_name(sym_idx_t i) {
  if (!i) {
    return "_";
  } else {
    std::ostringstream os;
    os << "SYM#" << i;
    return os.str();
  }
}

sym_idx_t SymTable::gen_lookup(std::string_view str, int mode, sym_idx_t idx) {
  unsigned long long h1 = 1, h2 = 1;
  for (char c : str) {
    h1 = ((h1 * 239) + (unsigned char)(c)) % SIZE_PRIME;
    h2 = ((h2 * 17) + (unsigned char)(c)) % (SIZE_PRIME - 1);
  }
  ++h2;
  ++h1;
  while (true) {
    if (sym[h1]) {
      if (sym[h1]->str == str) {
        return (mode & 2) ? not_found : sym_idx_t(h1);
      }
      h1 += h2;
      if (h1 > SIZE_PRIME) {
        h1 -= SIZE_PRIME;
      }
    } else {
      if (!(mode & 1)) {
        return not_found;
      }
      if (def_sym >= ((long long)SIZE_PRIME * 3) / 4) {
        throw SymTableOverflow{def_sym};
      }
      sym[h1] = std::make_unique<Symbol>(static_cast<std::string>(str), idx <= 0 ? sym_idx_t(h1) : -idx);
      ++def_sym;
      return sym_idx_t(h1);
    }
  }
}

void open_scope(SrcLocation loc) {
  ++scope_level;
  scope_opened_at.push_back(loc);
}

void close_scope(SrcLocation loc) {
  if (!scope_level) {
    throw Fatal{"cannot close the outer scope"};
  }
  while (!symbol_stack.empty() && symbol_stack.back().first == scope_level) {
    SymDef old_def = symbol_stack.back().second;
    auto idx = old_def.sym_idx;
    symbol_stack.pop_back();
    SymDef* cur_def = sym_def[idx];
    assert(cur_def);
    assert(cur_def->level == scope_level && cur_def->sym_idx == idx);
    //std::cerr << "restoring local symbol `" << old_def.name << "` of level " << scope_level << " to its previous level " << old_def.level << std::endl;
    if (cur_def->value) {
      //std::cerr << "deleting value of symbol " << old_def.name << ":" << old_def.level << " at " << (const void*) it->second.value << std::endl;
      delete cur_def->value;
    }
    if (!old_def.level && !old_def.value) {
      delete cur_def;  // ??? keep the definition always?
      sym_def[idx] = nullptr;
    } else {
      cur_def->value = std::move(old_def.value);
      cur_def->level = old_def.level;
    }
    old_def.value = nullptr;
  }
  --scope_level;
  scope_opened_at.pop_back();
}

SymDef* lookup_symbol(sym_idx_t idx) {
  if (!idx) {
    return nullptr;
  }
  if (sym_def[idx]) {
    return sym_def[idx];
  }
  if (global_sym_def[idx]) {
    return global_sym_def[idx];
  }
  return nullptr;
}

SymDef* define_global_symbol(sym_idx_t name_idx, bool force_new, SrcLocation loc) {
  if (!name_idx) {
    return nullptr;
  }
  auto found = global_sym_def[name_idx];
  if (found) {
    return force_new && found->value ? nullptr : found;
  }
  found = global_sym_def[name_idx] = new SymDef(0, name_idx, loc);
#ifdef TOLK_DEBUG
  found->sym_name = found->name();
#endif
  return found;
}

SymDef* define_symbol(sym_idx_t name_idx, bool force_new, SrcLocation loc) {
  if (!name_idx) {
    return nullptr;
  }
  if (!scope_level) {
    return define_global_symbol(name_idx, force_new, loc);
  }
  auto found = sym_def[name_idx];
  if (found) {
    if (found->level < scope_level) {
      symbol_stack.push_back(std::make_pair(scope_level, *found));
      found->level = scope_level;
    } else if (found->value && force_new) {
      return nullptr;
    }
    found->value = 0;
    found->loc = loc;
    return found;
  }
  found = sym_def[name_idx] = new SymDef(scope_level, name_idx, loc);
  symbol_stack.push_back(std::make_pair(scope_level, SymDef{0, name_idx, loc}));
#ifdef TOLK_DEBUG
  found->sym_name = found->name();
  symbol_stack.back().second.sym_name = found->name();
#endif
  return found;
}

}  // namespace tolk
