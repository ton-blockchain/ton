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

#include "src-file.h"
#include <string>

namespace tolk {

AllSrcFiles pipeline_discover_and_parse_sources(const std::string& stdlib_filename, const std::string& entrypoint_filename);

void pipeline_register_global_symbols(const AllSrcFiles&);
void pipeline_convert_ast_to_legacy_Expr_Op(const AllSrcFiles&);

void pipeline_find_unused_symbols();
void pipeline_generate_fif_output_to_std_cout(const AllSrcFiles&);

} // namespace tolk
