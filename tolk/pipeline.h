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

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#pragma once

#include "fwd-declarations.h"
#include <string>

namespace tolk {

void pipeline_discover_and_parse_sources(const std::string& stdlib_filename, const std::string& entrypoint_filename);

void pipeline_register_global_symbols();
void pipeline_resolve_identifiers_and_assign_symbols();
void pipeline_resolve_types_and_aliases();
void pipeline_calculate_rvalue_lvalue();
void pipeline_infer_types_and_calls_and_fields();
void pipeline_check_inferred_types();
void pipeline_refine_lvalue_for_mutate_arguments();
void pipeline_check_rvalue_lvalue();
void pipeline_check_pure_impure_operations();
void pipeline_check_serialized_fields();
void pipeline_constant_folding();
void pipeline_optimize_boolean_expressions();
void pipeline_convert_ast_to_legacy_Expr_Op();

void pipeline_find_unused_symbols();
void pipeline_generate_fif_output_to_std_cout();

// these pipes also can be called per-function individually
// they are called for instantiated generics functions, when `f<T>` is deeply cloned as `f<int>`
FunctionPtr pipeline_register_instantiated_generic_function(FunctionPtr base_fun_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs);

void pipeline_resolve_identifiers_and_assign_symbols(FunctionPtr);
void pipeline_resolve_types_and_aliases(FunctionPtr);
void pipeline_calculate_rvalue_lvalue(FunctionPtr);
void pipeline_detect_unreachable_statements(FunctionPtr);
void pipeline_infer_types_and_calls_and_fields(FunctionPtr);

StructPtr pipeline_register_instantiated_generic_struct(StructPtr base_struct_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs);
void pipeline_resolve_identifiers_and_assign_symbols(StructPtr);
void pipeline_resolve_types_and_aliases(StructPtr);

AliasDefPtr pipeline_register_instantiated_generic_alias(AliasDefPtr base_alias_ref, AnyV cloned_v, std::string&& name, const GenericsSubstitutions* substitutedTs);
void pipeline_resolve_types_and_aliases(AliasDefPtr);

} // namespace tolk
