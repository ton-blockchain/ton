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
#include "tolk.h"
#include "ast.h"
#include "compiler-state.h"

/*
 *   This is the starting point of compilation pipeline.
 *   It parses Tolk files to AST, analyzes `import` statements and loads/parses imported files.
 *
 *   When it finishes, all files have been parsed to AST, and no more files will later be added.
 *   If a parsing error happens (invalid syntax), an exception is thrown immediately from ast-from-tokens.cpp.
 */

namespace tolk {

AnyV parse_src_file_to_ast(const SrcFile* file);

void pipeline_discover_and_parse_sources(const std::string& stdlib_filename, const std::string& entrypoint_filename) {
  G.all_src_files.locate_and_register_source_file(stdlib_filename, {});
  G.all_src_files.locate_and_register_source_file(entrypoint_filename, {});

  while (SrcFile* file = G.all_src_files.get_next_unparsed_file()) {
    tolk_assert(!file->ast);

    file->ast = parse_src_file_to_ast(file);
    // if (!file->is_stdlib_file()) file->ast->debug_print();

    for (AnyV v_toplevel : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_import = v_toplevel->try_as<ast_import_directive>()) {
        std::string imported_str = v_import->get_file_name();
        size_t cur_slash_pos = file->rel_filename.rfind('/');
        std::string rel_filename = cur_slash_pos == std::string::npos || imported_str[0] == '@'
          ? std::move(imported_str)
          : file->rel_filename.substr(0, cur_slash_pos + 1) + imported_str;

        const SrcFile* imported = G.all_src_files.locate_and_register_source_file(rel_filename, v_import->loc);
        file->imports.push_back(SrcFile::ImportDirective{imported});
        v_import->mutate()->assign_src_file(imported);
      }
    }
  }

  // todo #ifdef TOLK_PROFILING
  // lexer_measure_performance(G.all_src_files);
}

} // namespace tolk
