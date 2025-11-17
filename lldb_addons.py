# noinspection PyUnresolvedReferences
import lldb


def call_cpp_debug_print(valobj, arg_type: str):
    type = valobj.GetType()
    if type.is_reference or type.is_pointer:
        addr = valobj.Dereference().GetAddress()
    else:
        addr = valobj.GetAddress()
    if not addr.IsValid():
        return "nullptr"

    s = valobj.GetFrame().EvaluateExpression('debug_print(({}*){})'.format(arg_type, addr)).GetSummary()
    s = str(s)[1:-1]  # trim quotes

    if "{enum.Op.cl}" in s:
        s = s.replace("{enum.Op.cl}", valobj.GetChildMemberWithName("cl").GetValue())
    if "{enum.AsmOp.t}" in s:
        s = s.replace("{enum.AsmOp.t}", valobj.GetChildMemberWithName("t").GetValue())
    return s


def print_td_RefInt256(valobj, internal_dict, options):
    n = valobj.EvaluateExpression("ptr.value.n").GetValueAsUnsigned()
    if n == 0:
        return "0"
    if n == 1:
        return valobj.EvaluateExpression("ptr.value.digits[0]").GetValueAsUnsigned()
    return "n=" + str(n)


def print_ast_vertex(valobj, internal_dict, options):
    type = valobj.GetType()
    if type.is_reference or type.is_pointer:
        addr = valobj.Dereference().GetAddress()
    else:
        addr = valobj.GetAddress()
    if not addr.IsValid():
        return "nullptr"

    s = valobj.GetFrame().EvaluateExpression('debug_print((tolk::ASTNodeBase*){})'.format(addr)).GetSummary()
    s = str(s)[1:-1]  # trim quotes

    return s


def __lldb_init_module(debugger, _):
    types_with_debug_print = [
        'tolk::Op',
        'tolk::TypeData',
        'tolk::VarDescr',
        'tolk::TmpVar',
        'tolk::VarDescrList',
        'tolk::AsmOp',
        'tolk::AsmOpList',
        'tolk::Stack',
        'tolk::SrcRange',
        'tolk::LocalVarData',
        'tolk::FunctionData',
        'tolk::GlobalVarData',
        'tolk::GlobalConstData',
        'tolk::AliasDeclarationData',
        'tolk::StructFieldData',
        'tolk::StructData',
        'tolk::EnumMemberData',
        'tolk::EnumDefData',
        'tolk::FlowContext',
        'tolk::SinkExpression',
        'tolk::InfoAboutExpr',
        'tolk::GenericsDeclaration',
        'tolk::GenericsSubstitutions',
    ]
    for arg_type in types_with_debug_print:
        debugger.HandleCommand('type summary add --python-script "return lldb_addons.call_cpp_debug_print(valobj, \'{}\')" {}'.format(arg_type, arg_type))

    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTNodeBase')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTNodeDeclaredTypeBase')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTNodeExpressionBase')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTNodeStatementBase')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTTypeLeaf')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTTypeVararg')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTExprLeaf')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTExprUnary')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTExprBinary')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTExprVararg')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTStatementUnary')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" tolk::ASTStatementVararg')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" -x "^tolk::V<.+>$"')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_ast_vertex" -x "^tolk::Vertex<.+>$"')
    debugger.HandleCommand('type summary add --python-function "lldb_addons.print_td_RefInt256" td::RefInt256')
