#include "tolk.h"
#include <ast.h>
#include <compiler-state.h>
#include "ast-stringifier.h"

namespace tolk {

void insert_debug_info(SrcLocation loc, ASTNodeKind kind, CodeBlob& code, size_t line_offset, std::string descr) {
  if (!G.settings.collect_source_map) {
    return;
  }

  if (kind == ast_artificial_aux_vertex || kind == ast_throw_statement) {
    return;
  }

#ifdef TOLK_DEBUG
  const auto last_op = std::find_if(code._vector_of_ops.rbegin(), code._vector_of_ops.rend(), [](const auto& it) {
    return it->cl != Op::_DebugInfo;
  });
  const Op* last_op_ptr = last_op != code._vector_of_ops.rend() ? *last_op : nullptr;
#endif

  auto& op = code.emplace_back(loc, Op::_DebugInfo);
  op.source_map_entry_idx = G.source_map.size();

  auto info = SourceMapEntry{};
  info.idx = op.source_map_entry_idx;
  info.descr = descr;
  info.is_entry = kind == ast_function_declaration;

#ifdef TOLK_DEBUG
  if (last_op_ptr) {
    std::stringstream st;
    last_op_ptr->show(st, code.vars, "", 4);

    info.opcode = st.str();
  }
#endif

  info.ast_kind = ASTStringifier::ast_node_kind_to_string(kind);

  if (const SrcFile* src_file = loc.get_src_file(); src_file != nullptr) {
    const auto& pos = src_file->convert_offset(loc.get_char_offset());

    info.loc.file = src_file->realpath;
    info.loc.offset = loc.get_char_offset();
    info.loc.line = pos.line_no;
    info.loc.line_offset = line_offset;
    info.loc.col = pos.char_no - 1;
    info.loc.length = 1; // Once we have the actual length of node, we should use it here
  }

  info.func_name = code.fun_ref->name;
  if (code.name != info.func_name) {
    // If a function was inlined, `code.name` will contain the name of the function we are inlining into
    info.inlined_to_func_name = code.name;
  }
  info.func_inline_mode = code.fun_ref->inline_mode;

  G.source_map.push_back(info);
}

void insert_debug_info(AnyV v, CodeBlob& code) {
  insert_debug_info(v->loc, v->kind, code, 0, "");
}

}
