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
#include "tolk.h"
#include "ast.h"
#include "ast-visitor.h"

/*
 *    This pipe does two things:
 * 1) detects unreachable code and prints warnings about it
 *    example: `fun main() { if(1){return;}else{return;} var x = 0; }` — var is unreachable
 * 2) if control flow reaches end of function, store a flag to insert an implicit return
 *    example: `fun main() { assert(...); }` — has an implicit `return ()` statement before a brace
 *
 *   Note, that it does not delete unreachable code, only prints warnings.
 *   Actual deleting is done much later (in "legacy" part), after AST is converted to Op.
 *
 *   Note, that it's not CFG, it's just a shallow reachability detection.
 *   In the future, a true CFG should be introduced. For instance, in order to have nullable types,
 * I'll need to implement smart casts. Then I'll think of a complicated granular control flow graph,
 * considering data flow and exceptions (built before type inferring, of course),
 * and detecting unreachable code will be a part of it.
 */

namespace tolk {

class UnreachableStatementsDetectVisitor final {
  bool always_returns(AnyV v) {
    switch (v->type) {
      case ast_sequence:              return always_returns(v->as<ast_sequence>());
      case ast_return_statement:      return always_returns(v->as<ast_return_statement>());
      case ast_throw_statement:       return always_returns(v->as<ast_throw_statement>());
      case ast_function_call:         return always_returns(v->as<ast_function_call>());
      case ast_repeat_statement:      return always_returns(v->as<ast_repeat_statement>());
      case ast_while_statement:       return always_returns(v->as<ast_while_statement>());
      case ast_do_while_statement:    return always_returns(v->as<ast_do_while_statement>());
      case ast_try_catch_statement:   return always_returns(v->as<ast_try_catch_statement>());
      case ast_if_statement:          return always_returns(v->as<ast_if_statement>());
      default:
        // unhandled statements (like assert) and statement expressions
        return false;
    }
  }

  bool always_returns(V<ast_sequence> v) {
    bool always = false;
    for (AnyV item : v->get_items()) {
      if (always && item->type != ast_empty_statement) {
        item->loc.show_warning("unreachable code");
        break;
      }
      always |= always_returns(item);
    }
    return always;
  }

  static bool always_returns([[maybe_unused]] V<ast_return_statement> v) {
    // quite obvious: `return expr` interrupts control flow
    return true;
  }

  static bool always_returns([[maybe_unused]] V<ast_throw_statement> v) {
    // todo `throw excNo` currently does not interrupt control flow
    // (in other words, `throw 1; something` - something is reachable)
    // the reason is that internally it's transformed to a call of built-in function __throw(),
    // which is a regular function, like __throw_if() or loadInt()
    // to fix this later on, it should be deeper, introducing Op::_Throw for example,
    // to make intermediate representations and stack optimizer also be aware that after it there is unreachable
    return false;
  }

  static bool always_returns([[maybe_unused]] V<ast_function_call> v) {
    // neither annotations like @noreturn nor auto-detection of always-throwing functions also doesn't exist
    // in order to do this in the future, it should be handled not only at AST/CFG level,
    // but inside Op and low-level optimizer (at least if reachability detection is not moved out of there)
    // see comments for `throw` above, similar to this case
    return false;
  }

  bool always_returns(V<ast_repeat_statement> v) {
    return always_returns(v->get_body());
  }

  bool always_returns(V<ast_while_statement> v) {
    return always_returns(v->get_body());
  }

  bool always_returns(V<ast_do_while_statement> v) {
    return always_returns(v->get_body());
  }

  bool always_returns(V<ast_try_catch_statement> v) {
    return always_returns(v->get_try_body()) && always_returns(v->get_catch_body());
  }

  bool always_returns(V<ast_if_statement> v) {
    return always_returns(v->get_if_body()) && always_returns(v->get_else_body());
  }

public:
  static bool should_visit_function(const FunctionData* fun_ref) {
    return fun_ref->is_code_function() && !fun_ref->is_generic_function();
  }

  void start_visiting_function(const FunctionData* fun_ref, V<ast_function_declaration> v_function) {
    bool control_flow_reaches_end = !always_returns(v_function->get_body()->as<ast_sequence>());
    if (control_flow_reaches_end) {
      fun_ref->mutate()->assign_is_implicit_return();
    }
  }
};


void pipeline_detect_unreachable_statements() {
  visit_ast_of_all_functions<UnreachableStatementsDetectVisitor>();
}

void pipeline_detect_unreachable_statements(const FunctionData* fun_ref) {
  UnreachableStatementsDetectVisitor visitor;
  if (UnreachableStatementsDetectVisitor::should_visit_function(fun_ref)) {
    visitor.start_visiting_function(fun_ref, fun_ref->ast_root->as<ast_function_declaration>());
  }
}

} // namespace tolk
