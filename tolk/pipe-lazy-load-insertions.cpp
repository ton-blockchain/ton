/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "lazy-helpers.h"
#include "ast.h"
#include "ast-aux-data.h"
#include "ast-visitor.h"
#include "ast-replacer.h"
#include "compilation-errors.h"
#include "type-system.h"
#include "smart-casts-cfg.h"
#include "pack-unpack-api.h"

namespace tolk {

/*
 *   This pipe finds `lazy` operators and inserts loading points to load only required fields just before being used.
 *   It happens after type inferring/checking. While inferring, `lazy expr` was inferred just as expr.
 * There is no dedicated `Lazy<T>` type in the type system. All the magic of laziness is calculated here.
 *
 *   This is the second version of the algorithm. The first one (which didn't reach production) attempted to
 * calculate precise loading locations right before every field usage.
 * E.g., `assert(obj.f1 > 0); assert(obj.f2 > 0)` — it detected "load f1" + assert1 + "load f2" + assert2.
 * However, it turned out to increase gas consumption. In practice, when a structure has only a few fields (99% cases),
 * it's better to load them ALL AT ONCE rather than on demand: this results in fewer stack manipulations.
 *
 *   `lazy` is not about lazy/partial loading, but also about partial updating.
 *   As opposed to non-lazy `var st = Storage.fromCell(c); st.xxx = yyy; st.toCell()`, which loads and writes all fields,
 * partial updating detects immutable portions of a struct, saves them separately, and reuses on toCell().
 *
 *   All in all, the algorithm focuses on:
 * - Identifying which fields are used for a lazy variable.
 * - Loading all required fields at once (and skipping unused ones).
 * - Doing lazy matching for unions, to avoid constructing heavy union types on the stack.
 * - Calculating and loading used fields at the right place for every union variant.
 * - Analyzing modification and gathering immutable portions at loading to be reused on saving.
 *
 *   Example "lazy union":
 *   > val msg = lazy MyMessage.fromSlice(msgBody);    // doesn't construct a union, actually
 *   > match (msg) {               // not a match by type, but a lazy match by a slice prefix
 *   >     CounterReset => {
 *   >         assert(senderAddress == storage.owner) throw 403;
 *   >         // <-- here "load msg.initial" is inserted
 *   >         storage.counter = msg.initial;
 *   >     }
 *   > }
 *
 *   Example "skip unused":
 *   > get seqno() {
 *   >     val storage = lazy Storage.fromCell(contract.getData());
 *   >     // <-- here "skip all fields before seqno, load seqno" is inserted
 *   >     return storage.seqno;
 *   > }
 *
 *   Example "nesting into try/if":
 *   > val st = lazy Storage.fromSlice(s);
 *   > try {
 *   >     // <-- here "load necessary fields" is inserted, if they are used only in `try` body
 *   >     st.someField
 *   > }
 *
 *   Example "bubbling to closest (lca) statement":
 *   > val st = lazy Storage.fromSlice(s);
 *   > // <-- here "load necessary fields" is inserted, because they are used both in `try` and after
 *   > try { ... st.someField } catch {}
 *   > st.anotherField
 *
 *   Example "modification and immutable tail":
 *   > val st = lazy loadStorage();
 *   > // <-- here "load f1 f2, save immutable tail, load rest" is inserted
 *   > ... read all fields
 *   > st.f2 = newValue;                // only f2 is modified, others are only read
 *   > contract.setData(st.toCell());   // st.toCell() writes f1 f2 and immutable tail
 *
 *   Example "modification and immutable gap":
 *   > struct Storage { a: int32; b: int32; c: int32; seqno: int32; }
 *   > var st = lazy loadStorage();
 *   > // <-- here "load 96 bits, load seqno" is inserted (note: "abc" are grouped into "96 bits")
 *   > st.seqno += 1;      // only seqno is accessed, "abc" not, that's why they are grouped
 *   > st.toCell();        // writes 96 bits (grouped "abc") and seqno
 *
 *   Implementation: "original struct" and "hidden struct".
 *   To group fields for loading/skipping, for every lazy variable, "hidden struct" is created, containing:
 *   - fields from original struct that are used
 *   - gaps and artificial fields that are not used, to match binary representation
 *
 *   Example 1:
 *   | struct Point { x: int8, y: int8 }    | hidden: struct lazyPoint { gap: bits8; y: int8 }
 *   | val p = lazy Point                   | p is initially `null null` on a stack
 *   | <-- "skip 8 bits, load y"            | field gap skipped, y loaded AND mapped onto a stack to match p.y
 *   | p.y                                  | p is now `null yValue`
 *
 *   Example 2:
 *   | struct St { a,b,c; seqno; ... }      | hidden: struct lazyStorage { gap: bits96; seqno: int32; tail: slice }
 *   | val st = lazy St                     | st is initially `null null null null`
 *   | <-- "load 96 bits, seqno, save tail" | field gap loaded, seqno loaded, tail (rest fields) saved
 *   | st.seqno += 1                        | st is now `null null null seqno`, "gap" and "tail" kept aside st's ir_idx
 *   | st.toCell()                          | writes gap (96 bits, grouped "abc") + modified seqno + storeSlice tail
 *
 *   In a similar way, it works for unions: `lazy UnionType` is represented exactly as `UnionType` on a stack,
 * that's why type transitions and methods inlining work natively (when transforming AST to Ops).
 * But for each variant, its own "lazyVariant" hidden_struct is created. Used fields are loaded and placed
 * into correct placed on a stack, gaps are skipped or placed aside.
 *
 *   Some highlights and considerations:
 *   - `lazy A.fromSlice(s)` does NOT read from slice immediately; instead, it saves the slice pointer and reads on demand
 *     (at "loading points" inserted by the compiler in the current pipe).
 *   - Options can be passed: `lazy A.fromSlice(s, {...})`, but `assertEndAfterReading` is ignored, it doesn't make sense
 *     (because fields are read later, only required ones, and the last is preloaded rather than loaded).
 *     There is a special method `a.forceLoadLazyObject()`, can be used inside `match` to load the variant fully.
 *   - The compiler detects which properties are accessed and inserts "load x y z" as close to the first usage as possible;
 *     it does NOT split loading into multiple instructions (first x y, then z somewhere below): it has a negative effect.
 *   - Inlined methods preserve laziness (e.g. `point.getX()` / `storage.save()`): the compiler analyzes the bodies of
 *     those methods to detect usages of `self`, and marks used fields to be loaded in advance.
 *   - Perfectly works for unions if a union is used only in `match`; then a union is not even created on a stack:
 *     instead, this `match` becomes lazy and works but cutting a slice prefix.
 *   - Effective toCell(): the compiler tracks which fields are modified and reuses an immutable tail slice on writing.
 *   - Has optimization "to reach a ref, no need to load preceding bits".
 *     E.g. `struct St { v: int32; content: cell; }` and `st = lazy St; st.content` does only LDREF, no "skip 32 bits".
 *   - The last requested field is preloaded, not loaded.
 *
 *   There are some drawbacks (probable possible enhancements):
 *   - When a union is used in some way except match, lazy is inapplicable (compilation error).
 *   - When a union has some primitives besides structures, lazy is inapplicable (for `T?`, particularly).
 *     Possible enhancement: handle unions where structs are mixed with primitives.
 *   - When a struct has a union field, it can be lazily matched only if it's the last.
 *     Possible enhancement: allow lazy match for union fields in the middle.
 *   - When `match` is used inside complex expressions, it's not lazy for safety.
 *     Possible enhancement: `cond && match(...)` is unsafe to be lazy, but `1 + match(...)` could be lazy.
 *   - Only methods preserve laziness (`p.getX()`), functions do not.
 *     Possible enhancement: `getXOfPoint(p)` could also be lazy for inlined functions (now `p` is read as a whole).
 */

// Given fun_ref = "A.fromSlice" from `lazy A.fromSlice(s)` check it's correct to be inside the `lazy` operator.
static bool does_function_satisfy_for_lazy_operator(FunctionPtr fun_ref, bool allow_wrapper = true) {
  if (!fun_ref) {
    return false;
  }
  // allow `lazy SomeStruct.fromSlice(s)`; these functions are also handled while transforming AST to Ops
  if (fun_ref->is_builtin() && fun_ref->is_instantiation_of_generic_function()) {
    std::string_view f_name = fun_ref->base_fun_ref->name;
    return f_name == "T.fromSlice" || f_name == "T.fromCell" || f_name == "Cell<T>.load";
  }
  // allow `lazy loadData()`, where loadData() is a simple wrapper like
  // `fun loadData() { return SomeStruct.fromCell(contract.getData()) }`
  if (allow_wrapper && fun_ref->is_code_function() && fun_ref->get_num_params() == 0) {
    auto f_body = fun_ref->ast_root->as<ast_function_declaration>()->get_body()->as<ast_block_statement>();
    if (f_body->size() == 1) {
      if (auto f_returns = f_body->get_item(0)->try_as<ast_return_statement>(); f_returns && f_returns->has_return_value()) {
        if (auto f_returns_call = f_returns->get_return_value()->try_as<ast_function_call>()) {
          return does_function_satisfy_for_lazy_operator(f_returns_call->fun_maybe, false);
        }
      }
    }
  }
  return false;
}

// Currently, only `A | B | ...` (only structures) can be lazily loaded; later steps rely on StructPtr.
// For example, `(int32, int32) | ...` or `T?` are incompatible with `lazy`.
// If structs don't have prefixes, a prefix tree is built for a union, it also works.
static TypePtr is_union_type_prevented_from_lazy_loading(const TypeDataUnion* t_union) {
  for (TypePtr variant : t_union->variants) {
    bool is_struct = variant->unwrap_alias()->try_as<TypeDataStruct>();
    if (!is_struct) {
      return variant;
    }
  }
  return nullptr;
}

// Given `lazy <expr>`, check that expr is correct: a valid function call with valid types.
// If not, fire an error.
static void check_lazy_operator_used_correctly(FunctionPtr cur_f, V<ast_lazy_operator> v) {
  bool is_ok_call = v->get_expr()->kind == ast_function_call
                 && does_function_satisfy_for_lazy_operator(v->get_expr()->as<ast_function_call>()->fun_maybe);
  if (!is_ok_call) {
    err("`lazy` operator can only be used with built-in functions like fromCell/fromSlice or simple wrappers over them").fire(v->keyword_range(), cur_f);
  }

  // it should be either a struct or a union of structs
  TypePtr expr_type = v->inferred_type;
  if (expr_type->unwrap_alias()->try_as<TypeDataStruct>()) {
    return;
  }
  if (const TypeDataUnion* expr_union = expr_type->unwrap_alias()->try_as<TypeDataUnion>()) {
    if (TypePtr wrong_variant = is_union_type_prevented_from_lazy_loading(expr_union)) {
      err("`lazy` union should contain only structures, but it contains `{}`", wrong_variant).fire(v->keyword_range(), cur_f);
    }
    return;
  }
  err("`lazy` is applicable to structs, not to `{}`", expr_type).fire(v->keyword_range(), cur_f);
}

// Given `storage.save()` for a lazy `storage` variable, check if `self` inside should gain laziness.
// If yes, the body of the method is also traversed to detect usages.
// If no, it's assumed that all fields of `storage` are used (an object used "as a whole").
static bool can_method_be_inlined_preserving_lazy(FunctionPtr method_ref) {
  return method_ref->is_inlined_in_place() &&    // only AST-inlined methods can be lazy
        !method_ref->has_mutate_params() &&
        !method_ref->does_return_self();
}

// The first stage of an algorithm is to collect every lazy expression, every field, every union variant.
// This "collecting" is done inside a block, considering all nested statements.
// As a result, we know how many times a variable (and every field independently) is used for reading, writing, etc.
struct ExprUsagesWhileCollecting {
  std::string name_str;       // "v" / "v.field" / "v.field.nested"; for debugging only
  TypePtr expr_type;          // either type of variable/field or its narrowed type inside `match`
  StructPtr struct_ref;       // if it's a struct, otherwise, nullptr

  int used_for_reading = 0;
  int used_for_writing = 0;
  int used_for_matching = 0;
  int used_for_toCell = 0;
  int used_reassigned_type = 0;
  int total_usages_with_fields = 0;
  std::vector<AnyV> needed_above_stmt;
  std::vector<V<ast_match_expression>> used_as_match_subj;

  std::vector<ExprUsagesWhileCollecting> fields;      // for struct: every field; otherwise: empty
  std::vector<ExprUsagesWhileCollecting> variants;    // for union: every variant; otherwise: itself (for match over non-union)

  ExprUsagesWhileCollecting(std::string name_for_debugging, TypePtr expr_type, bool is_variant_of_itself = false)
    : name_str(std::move(name_for_debugging))
    , expr_type(expr_type)
    , struct_ref(nullptr) {
    if (const TypeDataUnion* expr_union = expr_type->unwrap_alias()->try_as<TypeDataUnion>()) {
      variants.reserve(expr_union->size());
      for (int i = 0; i < expr_union->size(); ++i) {
        variants.emplace_back(name_str + "(#" + std::to_string(i) + ")", expr_union->variants[i]);
      }
      return;
    }
    if (const TypeDataStruct* t_struct = expr_type->unwrap_alias()->try_as<TypeDataStruct>()) {
      struct_ref = t_struct->struct_ref;
      fields.reserve(struct_ref->get_num_fields());
      for (int i = 0; i < struct_ref->get_num_fields(); ++i) {
        StructFieldPtr field_ref = struct_ref->get_field(i);
        fields.emplace_back(name_str + "." + field_ref->name, field_ref->declared_type);
      }
    }
    // to allow code like
    // > val msg = lazy Counter.fromSlice(s)       <-- struct! not union
    // > match (msg) { Counter => {} else => {} }
    // we track `msg` inside `match` as a single variant — not over union, but over itself
    if (!is_variant_of_itself) {
      variants.emplace_back(name_str, expr_type, true);
    }
  }

  void merge_with_sub_block(const ExprUsagesWhileCollecting& rhs) {
    tolk_assert(expr_type->equal_to(rhs.expr_type) && struct_ref == rhs.struct_ref);
    used_for_reading += rhs.used_for_reading;
    used_for_writing += rhs.used_for_writing;
    used_for_matching += rhs.used_for_matching;
    used_for_toCell += rhs.used_for_toCell;
    used_reassigned_type += rhs.used_reassigned_type;
    total_usages_with_fields += rhs.total_usages_with_fields;
    needed_above_stmt.insert(needed_above_stmt.end(), rhs.needed_above_stmt.begin(), rhs.needed_above_stmt.end());
    used_as_match_subj.insert(used_as_match_subj.end(), rhs.used_as_match_subj.begin(), rhs.used_as_match_subj.end());
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
      fields[i].merge_with_sub_block(rhs.fields[i]);
    }
    for (int i = 0; i < static_cast<int>(variants.size()); ++i) {
      variants[i].merge_with_sub_block(rhs.variants[i]);
    }
  }

  void on_used_rw(bool is_lvalue) {
    is_lvalue ? used_for_writing++ : used_for_reading++;
    total_usages_with_fields++;
  }

  void on_used_toCell() {
    used_for_toCell++;
    total_usages_with_fields++;
  }

  void on_used_as_match_subj(V<ast_match_expression> v_match) {
    used_as_match_subj.push_back(v_match);
    used_for_matching++;
    total_usages_with_fields++;
  }

  void on_used_reassigned_type() {
    used_reassigned_type++;    
  }

  bool is_self_or_field_used_for_reading() const {
    if (used_for_reading) {
      return true;
    }
    bool any = false;
    for (const ExprUsagesWhileCollecting& field_usages : fields) {
      any |= field_usages.is_self_or_field_used_for_reading() || field_usages.is_self_or_child_used_for_matching();
    }
    return any;
  }

  bool is_self_or_field_used_for_toCell() const {
    if (used_for_toCell) {
      return true;
    }
    bool any = false;
    for (const ExprUsagesWhileCollecting& field_usages : fields) {
      any |= field_usages.is_self_or_field_used_for_toCell();
    }
    return any;
  }

  bool is_self_or_child_used_for_writing() const {
    if (used_for_writing) {
      return true;
    }
    bool any = false;
    for (const ExprUsagesWhileCollecting& field_usages : fields) {
      any |= field_usages.is_self_or_child_used_for_writing();
    }
    for (const ExprUsagesWhileCollecting& variant_usages : variants) {
      any |= variant_usages.is_self_or_child_used_for_writing();
    }
    return any;
  }

  bool is_self_or_child_used_for_matching() const {
    if (used_for_matching) {
      return true;
    }
    bool any = false;
    for (const ExprUsagesWhileCollecting& field_usages : fields) {
      any |= field_usages.is_self_or_child_used_for_matching();
    }
    for (const ExprUsagesWhileCollecting& variant_usages : variants) {
      any |= variant_usages.is_self_or_child_used_for_matching();
    }
    return any;
  }

  void treat_match_like_read() {
    if (used_for_matching) {
      used_for_reading++;
      total_usages_with_fields++;
    }
    for (ExprUsagesWhileCollecting& field_usages : fields) {
      field_usages.treat_match_like_read();
    }
    for (ExprUsagesWhileCollecting& variant_usages : variants) {
      variant_usages.treat_match_like_read();
    }
  }

  LazyStructLoadInfo generate_hidden_struct_load_all(bool is_variant_of_union) const {
    tolk_assert(struct_ref);

    StructPtr hidden_struct = new StructData(
      "(lazy)" + struct_ref->name,
      struct_ref->ident_anchor,
      std::vector(struct_ref->fields),
      is_variant_of_union ? StructData::PackOpcode(0, 0) : struct_ref->opcode,
      struct_ref->overflow1023_policy,
      nullptr,
      nullptr,
      struct_ref->ast_root
    );
    std::vector all_fields_load_actions(struct_ref->get_num_fields(), LazyStructLoadInfo::LoadField);

    return LazyStructLoadInfo(struct_ref, hidden_struct, std::move(all_fields_load_actions));
  }

  // for every field of a struct, after calculating all usages, determine: which fields to load, and which to skip
  LazyStructLoadInfo calculate_hidden_struct(bool is_variant_of_union) const {
    tolk_assert(struct_ref);

    struct FutureField {
      LazyStructLoadInfo::ActionWithField action;
      std::string_view field_name;
      TypePtr field_type;
      PackSize pack_size;

      FutureField(LazyStructLoadInfo::ActionWithField action, std::string_view field_name, TypePtr field_type)
        : action(action), field_name(field_name), field_type(field_type)
        , pack_size(estimate_serialization_size(field_type)) {}
    };

    std::vector<FutureField> future_fields;

    bool object_used_as_a_whole = used_for_reading || used_for_writing || (used_for_toCell && is_variant_of_union);

    // if used as toCell(), detect last modified field_idx: after it, immutable tail can be saved
    bool need_immutable_tail = used_for_toCell && !is_variant_of_union;
    int last_modified_field_idx = -1;
    for (int field_idx = struct_ref->get_num_fields() - 1; field_idx >= 0; --field_idx) {
      if (used_for_writing || fields[field_idx].is_self_or_child_used_for_writing()) {
        last_modified_field_idx = field_idx;
        break;
      }
    }

    // fill future_fields
    for (int field_idx = 0; field_idx < struct_ref->get_num_fields(); ++field_idx) {
      StructFieldPtr orig_field = struct_ref->get_field(field_idx);
      TypePtr field_type = orig_field->declared_type;
      const ExprUsagesWhileCollecting& field_usages = fields[field_idx];
      bool used_anyhow_but_match = field_usages.is_self_or_field_used_for_reading() || field_usages.is_self_or_child_used_for_writing() || field_usages.is_self_or_field_used_for_toCell();

      if (need_immutable_tail && field_idx == last_modified_field_idx + 1) {
        future_fields.emplace_back(LazyStructLoadInfo::SaveImmutableTail, "(tail)", TypeDataSlice::create());
      }

      if (field_usages.used_for_matching == 1 && !used_anyhow_but_match && !object_used_as_a_whole && !used_for_toCell && !is_variant_of_union && field_idx == struct_ref->get_num_fields() - 1 &&
        field_type->unwrap_alias()->try_as<TypeDataUnion>() && !is_union_type_prevented_from_lazy_loading(field_type->unwrap_alias()->try_as<TypeDataUnion>())) {
        future_fields.emplace_back(LazyStructLoadInfo::LazyMatchField, orig_field->name, orig_field->declared_type);
        continue;
      }
      if (used_anyhow_but_match || field_usages.is_self_or_child_used_for_matching() || object_used_as_a_whole) {
        future_fields.emplace_back(LazyStructLoadInfo::LoadField, orig_field->name, orig_field->declared_type);
        continue;
      }

      // okay, this field is not needed; we should skip it;
      // try to merge "skip 8 bits" + "skip 16 bits" into a single "skip 24 bits"
      if (!future_fields.empty() && future_fields.back().action == LazyStructLoadInfo::SkipField) {
        if (const TypeDataBitsN* last_bitsN = future_fields.back().field_type->try_as<TypeDataBitsN>()) {
          PackSize cur_size = estimate_serialization_size(field_type);
          if (cur_size.min_bits == cur_size.max_bits && cur_size.max_refs == 0 && !cur_size.skipping_is_dangerous) {
            TypePtr total_bitsN = TypeDataBitsN::create(last_bitsN->n_width + cur_size.max_bits, true);
            future_fields.back().field_type = total_bitsN;
            continue;
          }
        }
      }

      // generate "skip 8 bits" instead of "skip int8" (it's more effective, and it can be merged with next)
      TypePtr skip_type = field_type;
      PackSize skip_size = estimate_serialization_size(field_type);
      if (skip_size.min_bits == skip_size.max_bits && skip_size.max_refs == 0 && !skip_size.skipping_is_dangerous) {
        skip_type = TypeDataBitsN::create(skip_size.max_bits, true);
      }
      future_fields.emplace_back(LazyStructLoadInfo::SkipField, "(gap)", skip_type);
    }

    // if we need tail, we should load all fields before it (even if they aren't used)
    if (need_immutable_tail) {
      for (FutureField& f : future_fields) {
        if (f.action == LazyStructLoadInfo::SaveImmutableTail) {
          break;
        }
        f.action = LazyStructLoadInfo::LoadField;
      }
    }

    // here we drop "skip field" if we actually don't need even to skip it, just ignore, like it does not exist;
    // example: unused fields in the end `load a; skip b; skip c` -> `load a`;
    // example: `skip bits8; load ref` - `load ref`, because to reach a ref, no need to skip preceding bits;
    for (size_t i = future_fields.size(); i-- > 0; ) {
      if (FutureField f = future_fields[i]; f.action == LazyStructLoadInfo::SkipField) {
        PackSize s_cur = f.pack_size;
        PackSize s_after(0);
        for (size_t j = i + 1; j < future_fields.size(); ++j) {
          s_after = EstimateContext::sum(s_after, future_fields[j].pack_size);
        }
        bool ignore = (s_after.max_bits == 0 && s_after.max_refs == 0)  // nothing is loaded after — no need to skip cur
                   || (s_after.max_bits == 0 && s_cur.max_refs == 0)    // no reach ref, no need to skip bits
                   || (s_after.max_refs == 0 && s_cur.max_bits == 0)    // and vice versa: no reach data, to need to skip refs
                   || (s_cur.max_bits == 0 && s_cur.max_refs == 0);     // empty struct/tensor, no need "bits0 skip"
        if (ignore) {
          future_fields.erase(future_fields.begin() + static_cast<int>(i));
        }
      }
    }

    // okay, we're done calculating; transform future_fields to hidden_struct
    std::vector<StructFieldPtr> hidden_fields;
    std::vector<LazyStructLoadInfo::ActionWithField> ith_field_action;
    hidden_fields.reserve(future_fields.size());
    ith_field_action.reserve(future_fields.size());
    for (int field_idx = 0; field_idx < static_cast<int>(future_fields.size()); ++field_idx) {
      FutureField f = future_fields[field_idx];
      AnyV v_ident = createV<ast_identifier>(SrcRange::undefined(), "");
      StructFieldPtr created = new StructFieldData(static_cast<std::string>(f.field_name), v_ident, field_idx, false, false, nullptr, nullptr);
      created->mutate()->assign_resolved_type(f.field_type);
      hidden_fields.push_back(created);
      ith_field_action.push_back(f.action);
    }

    StructPtr hidden_struct = new StructData(
      "(lazy)" + struct_ref->name,
      struct_ref->ident_anchor,
      std::move(hidden_fields),
      is_variant_of_union ? StructData::PackOpcode(0, 0) : struct_ref->opcode,
      struct_ref->overflow1023_policy,
      nullptr,
      nullptr,
      struct_ref->ast_root
    );

    return LazyStructLoadInfo(struct_ref, hidden_struct, std::move(ith_field_action));
  }
};

// After collecting all vars/fields/variants usages, we should store, where exactly (in AST) which fields to load.
// Every insertion point is represented as this class, it's transformed to an AST auxiliary vertex by a replacer.
struct OneLoadingInsertionPoint {
  std::vector<AnyV> all_stmts_where_used;
  TypePtr union_variant;
  StructFieldPtr field_ref;
  LazyStructLoadInfo load_info;
  mutable bool was_inserted_to_ast = false;

  OneLoadingInsertionPoint(std::vector<AnyV>&& all_stmts_where_used, TypePtr union_variant, StructFieldPtr field_ref, LazyStructLoadInfo&& load_info)
    : all_stmts_where_used(std::move(all_stmts_where_used))
    , union_variant(union_variant)
    , field_ref(field_ref)
    , load_info(std::move(load_info)) {}

  void mark_inserted_to_ast() const {
    was_inserted_to_ast = true;
  }

  bool is_mentioned_in_stmt(AnyV stmt) const {
    return std::find(all_stmts_where_used.begin(), all_stmts_where_used.end(), stmt) != all_stmts_where_used.end();
  }
};

// Every `lazy` operator must be assigned to a variable: `var st = lazy getStorage()`.
// Then `st` is a lazy variable, for which all calculations are done, after which it's stored as this class.
struct LazyVarInFunction {
  LocalVarPtr var_ref;
  V<ast_lazy_operator> created_by_lazy_op;
  V<ast_match_expression> v_lazy_match_var_itself = nullptr;  // lazy `match` for the variable itself
  V<ast_match_expression> v_lazy_match_last_field = nullptr;  // lazy `match` for the last field of a struct
  std::vector<OneLoadingInsertionPoint> load_points;          // a set of points where AST should be updated

  // convert already calculated usages of "st" variable and all its fields to a final immutable representation
  LazyVarInFunction(FunctionPtr cur_f, LocalVarPtr var_ref, V<ast_lazy_operator> created_by_lazy_op, ExprUsagesWhileCollecting&& var_usages)
    : var_ref(var_ref)
    , created_by_lazy_op(created_by_lazy_op) {

    // handle if `msg` is used only in `match (msg) { ... }`
    // (it may even be not a union, just a struct with opcode, and `match` with `else`)
    bool used_only_as_match = var_usages.used_for_matching == 1 && !var_usages.used_for_reading && !var_usages.used_for_toCell && !var_usages.used_for_writing && !var_usages.used_reassigned_type;
    bool variants_not_reassigned = std::all_of(var_usages.variants.begin(), var_usages.variants.end(), [](const ExprUsagesWhileCollecting& variant_usages) { return !variant_usages.used_reassigned_type; });
    if (used_only_as_match && variants_not_reassigned) {
      v_lazy_match_var_itself = var_usages.used_as_match_subj.front();
      load_points.reserve(var_usages.variants.size());
      for (ExprUsagesWhileCollecting& variant_usages : var_usages.variants) {
        LazyStructLoadInfo load_info = variant_usages.calculate_hidden_struct(true);
        load_points.emplace_back(std::move(variant_usages.needed_above_stmt), variant_usages.expr_type, nullptr, std::move(load_info));
      }
      return;
    }

    // okay, variable is used not only as `match`;
    // prohibit this to a union: lazy union may only be matched, nothing more (`msg is A` etc. don't work)
    const TypeDataStruct* t_struct = var_ref->declared_type->unwrap_alias()->try_as<TypeDataStruct>();
    bool is_union = t_struct == nullptr;
    if (is_union && used_only_as_match) {
      err("`lazy` will not work here, because variable `{}` changes its type inside `match`\n""hint: probably, it's reassigned, or called a method with a different receiver", var_ref).fire(created_by_lazy_op->keyword_range(), cur_f);
    }
    if (is_union) {
      err("`lazy` will not work here, because variable `{}` is used in a non-lazy manner\n""hint: lazy union may be used only in `match` statement, exactly once", var_ref).fire(created_by_lazy_op->keyword_range(), cur_f);
    }

    // so, it's just a struct, `lazy Point`; we've already calculated all statements where its fields are used
    LazyStructLoadInfo load_info = var_usages.calculate_hidden_struct(false);
    bool is_lazy_match_last_field = !load_info.ith_field_action.empty() && load_info.ith_field_action.back() == LazyStructLoadInfo::LazyMatchField;
    load_points.emplace_back(std::move(var_usages.needed_above_stmt), nullptr, nullptr, std::move(load_info));

    // but probably, there is `match (lazyObj.lastField)` which is lazy;
    if (is_lazy_match_last_field) {
      StructFieldPtr field_ref = t_struct->struct_ref->fields.back();
      const ExprUsagesWhileCollecting& last_field_usages = var_usages.fields.back();
      v_lazy_match_last_field = last_field_usages.used_as_match_subj.front();
      // inside `match` over a field, loading locations were not detected: insert "load all fields" into every arm
      for (int i = 0; i < v_lazy_match_last_field->get_arms_count(); ++i) {
        if (auto v_arm = v_lazy_match_last_field->get_arm(i); v_arm->pattern_kind == MatchArmKind::exact_type) {
          TypePtr union_variant = v_arm->pattern_type_node->resolved_type;
          auto v_arm_body = v_arm->get_body()->get_block_statement();
          if (!v_arm_body->empty()) {
            const TypeDataUnion* t_union = field_ref->declared_type->unwrap_alias()->try_as<TypeDataUnion>();
            int variant_idx = t_union->get_variant_idx(union_variant);
            LazyStructLoadInfo load_all = last_field_usages.variants[variant_idx].generate_hidden_struct_load_all(true);
            load_points.emplace_back(std::vector{v_arm_body->get_item(0)}, union_variant, field_ref, std::move(load_all));
          }
        }
      }
    }
  }
};

static std::unordered_map<FunctionPtr, std::vector<LazyVarInFunction>> functions_with_lazy_vars;


static ExprUsagesWhileCollecting collect_expr_usages_in_block(std::string name_for_debugging, SinkExpression s_expr, TypePtr expr_type, V<ast_block_statement> v_block);

// This visitor finds usages of "v" / "v.field" / etc. in ONE statement or expression and populates lazy_expr data.
// For every struct, all its fields are also populated; for a union — all its variants.
// Since AST vertices don't have "parent_node", we need to remember some details while traversing top-down.
class CollectUsagesInStatementVisitor final : public ASTVisitorFunctionBody {
  AnyV cur_stmt;
  SinkExpression s_expr;
  ExprUsagesWhileCollecting* lazy_expr;
  V<ast_dot_access> parent_dot = nullptr;

  CollectUsagesInStatementVisitor(AnyV cur_stmt, SinkExpression s_expr, ExprUsagesWhileCollecting* lazy_expr)
    : cur_stmt(cur_stmt), s_expr(s_expr), lazy_expr(lazy_expr) {}

  void visit(V<ast_reference> v) override {
    if (extract_sink_expression_from_vertex(v) == s_expr) {
      bool is_subj_of_dot = parent_dot && parent_dot->is_target_struct_field() && parent_dot->get_obj() == v;
      if (!is_subj_of_dot) {
        lazy_expr->on_used_rw(v->is_lvalue);
      }
      if (!v->is_lvalue && !lazy_expr->expr_type->equal_to(v->inferred_type)) {
        lazy_expr->on_used_reassigned_type();     // e.g. in `A => ...` variable was reassigned and now is `B`
      }
    }
  }

  void visit(V<ast_dot_access> v) override {
    if (extract_sink_expression_from_vertex(v) == s_expr) {
      bool is_subj_of_dot = parent_dot && parent_dot->is_target_struct_field() && parent_dot->get_obj() == v;
      if (!is_subj_of_dot) {
        lazy_expr->on_used_rw(v->is_lvalue);
      }
    }
    auto backup = parent_dot;
    parent_dot = v;
    parent::visit(v);
    parent_dot = backup;
  }

  void visit(V<ast_function_call> v) override {
    FunctionPtr fun_ref = v->fun_maybe;
    if (fun_ref && fun_ref->does_accept_self() && v->dot_obj_is_self) {
      AnyExprV dot_obj = v->get_callee()->as<ast_dot_access>()->get_obj();
      if (extract_sink_expression_from_vertex(dot_obj) == s_expr) {
        // handle built-in functions specially
        if (fun_ref->is_builtin() && fun_ref->base_fun_ref->name == "T.toCell") {
          lazy_expr->on_used_toCell();
          return;
        }

        // if receiver is another type, e.g. `fun (A|B).method(self)`, called from `match (v) { A => v.method() }`
        if (!fun_ref->parameters[0].declared_type->equal_to(lazy_expr->expr_type)) {
          lazy_expr->on_used_reassigned_type();
        }
        // for `obj.f.method()`, mark lazy_expr=obj.f "used" anyway
        if (s_expr.index_path) {    
          lazy_expr->on_used_rw(false);
        }
        // if we have `st.save()` / `p.getX()` / `obj.f.method()`, which will be inlined when transforming to IR,
        // dig into that method's body to fetch used fields `self.x` etc.
        if (can_method_be_inlined_preserving_lazy(fun_ref)) {
          auto v_body_block = fun_ref->ast_root->try_as<ast_function_declaration>()->get_body()->try_as<ast_block_statement>();
          ExprUsagesWhileCollecting inner_usages = collect_expr_usages_in_block(lazy_expr->name_str + "(=self)", SinkExpression(&fun_ref->parameters[0]), lazy_expr->expr_type, v_body_block);
          inner_usages.treat_match_like_read();   // nested lazy match in inlined functions doesn't work, it's not wrapped into aux vertex
          lazy_expr->merge_with_sub_block(inner_usages);
          return;
        }
      }
    }

    parent::visit(v);
  }

  void visit(V<ast_match_expression> v) override {
    AnyExprV subj = v->get_subject();
    bool is_match_by_cur = extract_sink_expression_from_vertex(subj) == s_expr;

    // `match` statement over current expression is okay (it will be lazy if it's the only, and other conditions satisfy);
    // `match` expression, generally, is not safe to be lazy, e.g. `return cond && match(...)`,
    // but simply `return match(...)` / `var result = match(...)` is okay
    bool is_safe = false;
    if (v->is_statement()) {
      is_safe = cur_stmt == v;
    } else if (auto v_return = cur_stmt->try_as<ast_return_statement>()) {
      is_safe = v_return->get_return_value() == v;
    } else if (auto v_assign = cur_stmt->try_as<ast_assign>()) {
      is_safe = v_assign->get_rhs() == v;
    } else if (auto v_set_assign = cur_stmt->try_as<ast_set_assign>()) {
      is_safe = v_set_assign->get_rhs() == v;
    }

    if (!lazy_expr->expr_type->equal_to(subj->inferred_type)) {
      is_safe = false;    // `v = v as Union; match (v)` or inside a method with a different receiver
    }

    if (is_match_by_cur && is_safe) {
      lazy_expr->on_used_as_match_subj(v);
      const TypeDataUnion* expr_as_union = lazy_expr->expr_type->unwrap_alias()->try_as<TypeDataUnion>();
      for (int i = 0; i < v->get_arms_count(); ++i) {
        if (auto v_arm = v->get_arm(i); v_arm->pattern_kind == MatchArmKind::exact_type) {
          TypePtr exact_type = v_arm->pattern_type_node->resolved_type;
          auto v_block = v_arm->get_body()->get_block_statement();
          ExprUsagesWhileCollecting variant_usages = collect_expr_usages_in_block(lazy_expr->name_str, s_expr, exact_type, v_block);
          int variant_idx = expr_as_union ? expr_as_union->get_variant_idx(exact_type) : 0;   // match over non-union is ok
          lazy_expr->variants[variant_idx].merge_with_sub_block(variant_usages);
        }
      }
      return;
    }

    parent::visit(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    tolk_assert(false);
  }

  static void collect_usages_in_expression(ExprUsagesWhileCollecting* out, SinkExpression s_expr, AnyV v_expr) {
    CollectUsagesInStatementVisitor visitor(v_expr, s_expr, out);
    visitor.parent::visit(v_expr);
    if (out->struct_ref) {
      for (int field_idx = 0; field_idx < out->struct_ref->get_num_fields(); ++field_idx) {
        collect_usages_in_expression(&out->fields[field_idx], s_expr.get_child_s_expr(field_idx), v_expr);
        out->total_usages_with_fields += out->fields[field_idx].total_usages_with_fields;
      }
    }
  }
};

// This visitor analyzes A WHOLE BLOCK, statement by statement, and detects all statements where lazy_expr is used.
// It takes care of nested try/catch, etc.
// Ideally, it should calculate the only "lca" AST vertex of all usages, but it's not as easy as it seems.
// Instead, `lazy_expr->needed_above_stmt` contains all statements where expr is "mentioned" (and needs to be loaded before).
// And later, traversing top-down, the first occurrence is taken, inserting an AST aux vertex right before it.
class CollectUsagesInBlockBottomUp {
  ExprUsagesWhileCollecting* lazy_expr;
  SinkExpression s_expr;

  CollectUsagesInBlockBottomUp(ExprUsagesWhileCollecting* lazy_expr, SinkExpression s_expr)
    : lazy_expr(lazy_expr), s_expr(s_expr) {}

  void visit_try_catch_statement(V<ast_try_catch_statement> v) const {
    ExprUsagesWhileCollecting u_try = visit_sub_block(v->get_try_body());
    ExprUsagesWhileCollecting u_catch = visit_sub_block(v->get_catch_body());

    if (u_try.total_usages_with_fields && u_catch.total_usages_with_fields) {
      lazy_expr->needed_above_stmt.push_back(v);
    }
    if (u_try.total_usages_with_fields || u_catch.total_usages_with_fields) {
      if (lazy_expr->total_usages_with_fields) {
        lazy_expr->needed_above_stmt.push_back(v);
      }
    }

    lazy_expr->merge_with_sub_block(u_try);
    lazy_expr->merge_with_sub_block(u_catch);
  }

  void visit_if_statement(V<ast_if_statement> v) const {
    ExprUsagesWhileCollecting u_cond = visit_other(v->get_cond());
    ExprUsagesWhileCollecting u_then = visit_sub_block(v->get_if_body());
    ExprUsagesWhileCollecting u_else = visit_sub_block(v->get_else_body());

    if (u_cond.total_usages_with_fields) {
      lazy_expr->needed_above_stmt.push_back(v);
    }
    if (u_then.total_usages_with_fields && u_else.total_usages_with_fields) {
      lazy_expr->needed_above_stmt.push_back(v);
    }
    if (u_then.total_usages_with_fields || u_else.total_usages_with_fields) {
      if (lazy_expr->total_usages_with_fields) {
        lazy_expr->needed_above_stmt.push_back(v);
      }
    }

    lazy_expr->merge_with_sub_block(u_cond);
    lazy_expr->merge_with_sub_block(u_then);
    lazy_expr->merge_with_sub_block(u_else);
  }

  void visit_block_statement(V<ast_block_statement> v) const {
    for (int i = v->size() - 1; i >= 0; --i) {
      AnyV ith_statement = v->get_item(i);
      switch (ith_statement->kind) {
        case ast_try_catch_statement:
          visit_try_catch_statement(ith_statement->as<ast_try_catch_statement>());
          continue;
        case ast_if_statement:
          visit_if_statement(ith_statement->as<ast_if_statement>());
          continue;
        default:
          break;
      }

      ExprUsagesWhileCollecting u_ith = visit_other(ith_statement);
      if (u_ith.total_usages_with_fields) {
        u_ith.needed_above_stmt.push_back(ith_statement);
      }
      lazy_expr->merge_with_sub_block(u_ith);
    }
  }

  ExprUsagesWhileCollecting visit_sub_block(V<ast_block_statement> v_block) const {
    return visit_block_bottom_up(lazy_expr->name_str, s_expr, lazy_expr->expr_type, v_block);
  }

  ExprUsagesWhileCollecting visit_other(AnyV v) const {
    ExprUsagesWhileCollecting result(lazy_expr->name_str, lazy_expr->expr_type);
    CollectUsagesInStatementVisitor::collect_usages_in_expression(&result, s_expr, v);
    return result;
  }

public:
  static ExprUsagesWhileCollecting visit_block_bottom_up(std::string name_for_debugging, SinkExpression s_expr, TypePtr expr_type, V<ast_block_statement> v_block) {
    ExprUsagesWhileCollecting lazy_expr(std::move(name_for_debugging), expr_type);
    CollectUsagesInBlockBottomUp visitor(&lazy_expr, s_expr);
    visitor.visit_block_statement(v_block);
    return lazy_expr;
  }
};

static ExprUsagesWhileCollecting collect_expr_usages_in_block(std::string name_for_debugging, SinkExpression s_expr, TypePtr expr_type, V<ast_block_statement> v_block) {
  return CollectUsagesInBlockBottomUp::visit_block_bottom_up(std::move(name_for_debugging), s_expr, expr_type, v_block);
}

// Step 1:
// This visitor finds `var st = lazy expr`, launches finding usages for `st`,
// and adds `st` as LazyVarInFunction to a global list.
class CollectAllLazyObjectsAndFieldsVisitor final : public ASTVisitorFunctionBody {
  V<ast_block_statement> parent_block = nullptr;

  void visit(V<ast_block_statement> v) override {
    auto backup = parent_block;
    parent_block = v;
    parent::visit(v);
    parent_block = backup;
  }

  // `var st = lazy ...`
  void visit(V<ast_assign> v) override {
    if (auto rhs_lazy = v->get_rhs()->try_as<ast_lazy_operator>()) {
      check_lazy_operator_used_correctly(cur_f, rhs_lazy);

      if (auto lhs_var_decl = v->get_lhs()->try_as<ast_local_vars_declaration>()) {
        auto lhs_var = lhs_var_decl->get_expr()->try_as<ast_local_var_lhs>();
        if (!lhs_var->marked_as_redef) {
          // collect usages of a lazy var inside the same block statement where it's declared
          LocalVarPtr var_ref = lhs_var->var_ref;
          ExprUsagesWhileCollecting var_usages = collect_expr_usages_in_block(var_ref->name, SinkExpression(var_ref), var_ref->declared_type, parent_block);
          LazyVarInFunction lazy_var(cur_f, var_ref, rhs_lazy, std::move(var_usages));
          functions_with_lazy_vars[cur_f].emplace_back(std::move(lazy_var));
        }
      }
    }

    parent::visit(v);
  }

  // check that `lazy` operator used in a correct pattern with a correct expression
  void visit(V<ast_lazy_operator> v) override {
    for (const LazyVarInFunction& lazy_var : functions_with_lazy_vars[cur_f]) {
      if (lazy_var.created_by_lazy_op == v) {
        parent::visit(v);
        return;
      }
    }

    // for `return lazy ...` and other cases except allowed
    err("incorrect `lazy` operator usage, it's not directly assigned to a variable\n""hint: use `lazy` like this:\n> var st = lazy MyStorage.fromSlice(...)").fire(v->keyword_range(), cur_f);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }
};

// Step 2:
// After visiting all functions and finding all lazy variables, this replacer updates AST,
// inserting (already calculated) load vertices. They are auxiliary vertices holding special data.
// They are handled later when transforming AST to Ops.
class LazyLoadInsertionsReplacer final : public ASTReplacerInFunctionBody {

  // `var st = lazy expr` -> save "st" (it will be used in codegen to assert "st.x" that "x" is loaded)
  AnyExprV replace(V<ast_lazy_operator> v) override {
    for (const LazyVarInFunction& lazy_var : functions_with_lazy_vars[cur_f]) {
      if (lazy_var.created_by_lazy_op == v) {
        v->mutate()->assign_dest_var_ref(lazy_var.var_ref);
        return parent::replace(v);
      }
    }

    tolk_assert(false);     // all `lazy` operators where detected and handled
  }

  // `{ ... }` -> `{ ... load ... }`
  AnyV replace(V<ast_block_statement> v) override {
    std::vector<AnyV> new_children;       // since we don't have "parent_node" and "next_child" in AST,
    new_children.reserve(v->size());   // traverse every block statement and insert "load" in the middle

    for (AnyV stmt : v->get_items()) {
      for (const LazyVarInFunction& lazy_var : functions_with_lazy_vars[cur_f]) {
        for (const OneLoadingInsertionPoint& ins : lazy_var.load_points) {
          if (!ins.was_inserted_to_ast && ins.is_mentioned_in_stmt(stmt)) {
            ASTAuxData* aux_data = new AuxData_LazyObjectLoadFields(lazy_var.var_ref, ins.union_variant, ins.field_ref, ins.load_info);
            new_children.push_back(createV<ast_artificial_aux_vertex>(createV<ast_empty_expression>(stmt->range), aux_data, TypeDataVoid::create()));
            ins.mark_inserted_to_ast();
          }
        }
      }
      new_children.push_back(parent::replace(stmt));
    }

    v->mutate()->assign_new_children(std::move(new_children));
    return v;
  }

  // `match (lazy_obj)` / `match (lazy_obj.field)` -> wrap with aux
  AnyExprV replace(V<ast_match_expression> v) override {
    for (const LazyVarInFunction& lazy_var : functions_with_lazy_vars[cur_f]) {
      bool is_lazy_match_for_union = lazy_var.v_lazy_match_var_itself == v;
      if (is_lazy_match_for_union) {
        ASTAuxData* aux_data = new AuxData_LazyMatchForUnion(lazy_var.var_ref, nullptr);
        return createV<ast_artificial_aux_vertex>(parent::replace(v), aux_data, v->inferred_type);
      }

      bool is_lazy_match_for_last_field = lazy_var.v_lazy_match_last_field == v;
      if (is_lazy_match_for_last_field) {
        StructPtr struct_ref = lazy_var.var_ref->declared_type->unwrap_alias()->try_as<TypeDataStruct>()->struct_ref;
        ASTAuxData* aux_data = new AuxData_LazyMatchForUnion(lazy_var.var_ref, struct_ref->fields.back());
        return createV<ast_artificial_aux_vertex>(parent::replace(v), aux_data, v->inferred_type);
      }
    }

    return parent::replace(v);
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && functions_with_lazy_vars.contains(fun_ref);
  }
};

// Step 3:
// After modifying AST (inserting loads, lazy match, etc.),
// check __expect_lazy() calls, used in compiler tests as assertions.
class CheckExpectLazyAssertionsVisitor final : public ASTVisitorFunctionBody {

  static std::string stringify_lazy_load_above_stmt(const AuxData_LazyObjectLoadFields* aux_load) {
    static const char* action_to_str[] = {"load", "skip", "lazy match", "save immutable"};

    const LazyStructLoadInfo& load_info = aux_load->load_info;
    StructPtr struct_ref = load_info.hidden_struct;
    std::string_view last_action;

    std::string result = "[" + aux_load->var_ref->name + "] ";
    for (int i = 0; i < struct_ref->get_num_fields(); ++i) {
      std::string field_name = struct_ref->get_field(i)->name;
      if (field_name == "(gap)") {
        field_name = "(" + struct_ref->get_field(i)->declared_type->as_human_readable() + ")";
      }
      std::string_view action = action_to_str[load_info.ith_field_action[i]];
      if (action != last_action) {
        if (result[result.size() - 2] != ']') {
          result += ", ";
        }
        result += action;
        last_action = action;
      }
      result += " ";
      result += field_name;
    }
    return result;
  }

  void visit(V<ast_block_statement> v) override {
    // again, given "__expect_lazy(...)", we have no "next sibling", so traverse block statements
    for (int i = 0; i < v->size(); ++i) {
      AnyV cur_stmt = v->get_item(i);
      if (auto v_call = cur_stmt->try_as<ast_function_call>()) {
        if (v_call->fun_maybe && v_call->fun_maybe->is_builtin() && v_call->fun_maybe->name == "__expect_lazy") {
          // __expect_lazy("...") is a compiler built-in for testing, it's not indented to be called by users
          auto v_expected_str = v_call->get_arg(0)->get_expr()->try_as<ast_string_const>();
          tolk_assert(i + 1 < v->size() && v_expected_str && "invalid __expect_lazy");
          AnyV next_stmt = v->get_item(i + 1);
          std::string actual;
          if (auto next_aux = next_stmt->try_as<ast_artificial_aux_vertex>()) {
            if (const auto* aux_load = dynamic_cast<const AuxData_LazyObjectLoadFields*>(next_aux->aux_data)) {
              actual = stringify_lazy_load_above_stmt(aux_load);
            }
            if (const auto* aux_match = dynamic_cast<const AuxData_LazyMatchForUnion*>(next_aux->aux_data)) {
              actual = "[" + aux_match->var_ref->name + "] " + "lazy match";
            }
          }

          if (actual != v_expected_str->str_val) {
            err("__expect_lazy failed: actual \"{}\"", actual).fire(SrcRange::span(cur_stmt->range, 13));
          }
        }
      }
      parent::visit(cur_stmt);
    }
  }

public:
  bool should_visit_function(FunctionPtr fun_ref) override {
    return fun_ref->is_code_function() && functions_with_lazy_vars.contains(fun_ref);
  }
};

void pipeline_lazy_load_insertions() {
  visit_ast_of_all_functions<CollectAllLazyObjectsAndFieldsVisitor>();
  replace_ast_of_all_functions<LazyLoadInsertionsReplacer>();
  visit_ast_of_all_functions<CheckExpectLazyAssertionsVisitor>();
  functions_with_lazy_vars.clear();
}

} // namespace tolk
