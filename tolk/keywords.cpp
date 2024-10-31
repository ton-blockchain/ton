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

namespace tolk {

/*
 * 
 *   KEYWORD DEFINITION
 * 
 */

void define_keywords() {
  symbols.add_kw_char('+')
      .add_kw_char('-')
      .add_kw_char('*')
      .add_kw_char('/')
      .add_kw_char('%')
      .add_kw_char('?')
      .add_kw_char(':')
      .add_kw_char(',')
      .add_kw_char(';')
      .add_kw_char('(')
      .add_kw_char(')')
      .add_kw_char('[')
      .add_kw_char(']')
      .add_kw_char('{')
      .add_kw_char('}')
      .add_kw_char('=')
      .add_kw_char('_')
      .add_kw_char('<')
      .add_kw_char('>')
      .add_kw_char('&')
      .add_kw_char('|')
      .add_kw_char('^')
      .add_kw_char('~');

  symbols.add_keyword("==", Keyword::_Eq)
      .add_keyword("!=", Keyword::_Neq)
      .add_keyword("<=", Keyword::_Leq)
      .add_keyword(">=", Keyword::_Geq)
      .add_keyword("<=>", Keyword::_Spaceship)
      .add_keyword("<<", Keyword::_Lshift)
      .add_keyword(">>", Keyword::_Rshift)
      .add_keyword("~>>", Keyword::_RshiftR)
      .add_keyword("^>>", Keyword::_RshiftC)
      .add_keyword("~/", Keyword::_DivR)
      .add_keyword("^/", Keyword::_DivC)
      .add_keyword("~%", Keyword::_ModR)
      .add_keyword("^%", Keyword::_ModC)
      .add_keyword("/%", Keyword::_DivMod)
      .add_keyword("+=", Keyword::_PlusLet)
      .add_keyword("-=", Keyword::_MinusLet)
      .add_keyword("*=", Keyword::_TimesLet)
      .add_keyword("/=", Keyword::_DivLet)
      .add_keyword("~/=", Keyword::_DivRLet)
      .add_keyword("^/=", Keyword::_DivCLet)
      .add_keyword("%=", Keyword::_ModLet)
      .add_keyword("~%=", Keyword::_ModRLet)
      .add_keyword("^%=", Keyword::_ModCLet)
      .add_keyword("<<=", Keyword::_LshiftLet)
      .add_keyword(">>=", Keyword::_RshiftLet)
      .add_keyword("~>>=", Keyword::_RshiftRLet)
      .add_keyword("^>>=", Keyword::_RshiftCLet)
      .add_keyword("&=", Keyword::_AndLet)
      .add_keyword("|=", Keyword::_OrLet)
      .add_keyword("^=", Keyword::_XorLet);

  symbols.add_keyword("return", Keyword::_Return)
      .add_keyword("var", Keyword::_Var)
      .add_keyword("repeat", Keyword::_Repeat)
      .add_keyword("do", Keyword::_Do)
      .add_keyword("while", Keyword::_While)
      .add_keyword("until", Keyword::_Until)
      .add_keyword("try", Keyword::_Try)
      .add_keyword("catch", Keyword::_Catch)
      .add_keyword("if", Keyword::_If)
      .add_keyword("ifnot", Keyword::_Ifnot)
      .add_keyword("then", Keyword::_Then)
      .add_keyword("else", Keyword::_Else)
      .add_keyword("elseif", Keyword::_Elseif)
      .add_keyword("elseifnot", Keyword::_Elseifnot);

  symbols.add_keyword("int", Keyword::_Int)
      .add_keyword("cell", Keyword::_Cell)
      .add_keyword("slice", Keyword::_Slice)
      .add_keyword("builder", Keyword::_Builder)
      .add_keyword("cont", Keyword::_Cont)
      .add_keyword("tuple", Keyword::_Tuple)
      .add_keyword("type", Keyword::_Type)
      .add_keyword("->", Keyword::_Mapsto)
      .add_keyword("forall", Keyword::_Forall);

  symbols.add_keyword("extern", Keyword::_Extern)
      .add_keyword("global", Keyword::_Global)
      .add_keyword("asm", Keyword::_Asm)
      .add_keyword("impure", Keyword::_Impure)
      .add_keyword("pure", Keyword::_Pure)
      .add_keyword("inline", Keyword::_Inline)
      .add_keyword("inline_ref", Keyword::_InlineRef)
      .add_keyword("builtin", Keyword::_Builtin)
      .add_keyword("auto_apply", Keyword::_AutoApply)
      .add_keyword("method_id", Keyword::_MethodId)
      .add_keyword("get", Keyword::_Get)
      .add_keyword("operator", Keyword::_Operator)
      .add_keyword("infix", Keyword::_Infix)
      .add_keyword("infixl", Keyword::_Infixl)
      .add_keyword("infixr", Keyword::_Infixr)
      .add_keyword("const", Keyword::_Const);

  symbols.add_keyword("#pragma", Keyword::_PragmaHashtag)
      .add_keyword("#include", Keyword::_IncludeHashtag);
}

}  // namespace tolk
