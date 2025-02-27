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

namespace tolk {

struct ASTNodeBase;
struct ASTNodeDeclaredTypeBase;
struct ASTNodeExpressionBase;
struct ASTNodeStatementBase;

using AnyV = const ASTNodeBase*;
using AnyTypeV = const ASTNodeDeclaredTypeBase*;
using AnyExprV = const ASTNodeExpressionBase*;
using AnyStatementV = const ASTNodeStatementBase*;

struct Symbol;
struct LocalVarData;
struct FunctionData;
struct GlobalVarData;
struct GlobalConstData;
struct AliasDefData;
struct StructFieldData;
struct StructData;

using LocalVarPtr = const LocalVarData*;
using FunctionPtr = const FunctionData*;
using GlobalVarPtr = const GlobalVarData*;
using GlobalConstPtr = const GlobalConstData*;
using AliasDefPtr = const AliasDefData*;
using StructFieldPtr = const StructFieldData*;
using StructPtr = const StructData*;

class TypeData;
using TypePtr = const TypeData*;

struct GenericsSubstitutions;

struct SrcFile;

} // namespace tolk
