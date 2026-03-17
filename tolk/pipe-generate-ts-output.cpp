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

/**
 * TypeScript Codegen Pipeline Stage
 * 
 * This pipeline stage generates TypeScript interfaces and serialization functions
 * from Tolk struct and enum definitions. It runs after all type checking is complete
 * and produces @ton/core-compatible output.
 * 
 * Key design decisions:
 * - Uses @ton/core as the standard TON TypeScript library
 * - Generates load/store functions matching tlb-codegen conventions
 * - Supports opcodes, nullable fields, typed Cell<T> references
 * - Skips generic structs (only instantiated versions are generated)
 * - Skips private fields in generated API
 * 
 * Usage:
 *   tolk --emit-typescript contract.tolk
 *   # or via tolk-js: { "emitTypescript": true }
 */

#include "compiler-state.h"
#include "compiler-settings.h"
#include "type-system.h"
#include "ts-type-mapping.h"
#include <sstream>
#include <fstream>
#include <set>

namespace tolk {

// Collect all required imports based on types used
static void collect_imports(StructPtr struct_ref, 
                           bool& need_address, 
                           bool& need_cell,
                           bool& need_dictionary,
                           bool& need_bitstring) {
    for (int i = 0; i < struct_ref->get_num_fields(); i++) {
        StructFieldPtr field = struct_ref->get_field(i);
        if (field->is_private) continue;
        
        TsTypeInfo info = get_ts_type_info(field->declared_type, field->name);
        need_address |= info.needs_import_address;
        need_cell |= info.needs_import_cell;
        need_dictionary |= info.needs_import_dictionary;
        need_bitstring |= info.needs_import_bitstring;
    }
}

// Generate TypeScript interface for a struct
static std::string generate_struct_interface(StructPtr struct_ref) {
    std::ostringstream ss;
    
    ss << "export interface " << struct_ref->name << " {\n";
    
    // Add discriminator for structs with opcodes
    if (struct_ref->opcode.exists()) {
        ss << "    readonly $$type: '" << struct_ref->name << "';\n";
    }
    
    for (int i = 0; i < struct_ref->get_num_fields(); i++) {
        StructFieldPtr field = struct_ref->get_field(i);
        if (field->is_private) continue;
        
        TsTypeInfo type_info = get_ts_type_info(field->declared_type);
        
        // Add readonly for readonly fields
        if (field->is_readonly) {
            ss << "    readonly ";
        } else {
            ss << "    ";
        }
        
        ss << field->name << ": " << type_info.ts_type << ";\n";
    }
    
    ss << "}\n";
    return ss.str();
}

// Generate opcode constant if struct has one
static std::string generate_opcode_constant(StructPtr struct_ref) {
    if (!struct_ref->opcode.exists()) {
        return "";
    }
    
    std::ostringstream ss;
    ss << "export const " << struct_ref->name << "Opcode = 0x" 
       << std::hex << struct_ref->opcode.pack_prefix << std::dec << ";\n\n";
    return ss.str();
}

// Generate load function for a struct
static std::string generate_load_function(StructPtr struct_ref) {
    std::ostringstream ss;
    
    ss << "export function load" << struct_ref->name << "(slice: Slice): " << struct_ref->name << " {\n";
    ss << generate_load_function_body(struct_ref);
    ss << "}\n";
    
    return ss.str();
}

// Generate store function for a struct
static std::string generate_store_function(StructPtr struct_ref) {
    std::ostringstream ss;
    
    ss << "export function store" << struct_ref->name << "(src: " << struct_ref->name << "): (builder: Builder) => void {\n";
    ss << generate_store_function_body(struct_ref);
    ss << "}\n";
    
    return ss.str();
}

// Generate TypeScript enum
static std::string generate_enum(EnumDefPtr enum_ref) {
    std::ostringstream ss;
    
    ss << "export enum " << enum_ref->name << " {\n";
    
    for (size_t i = 0; i < enum_ref->members.size(); i++) {
        EnumMemberPtr member = enum_ref->members[i];
        ss << "    " << member->name;
        
        // Include explicit value if available
        if (member->computed_value.not_null()) {
            ss << " = " << member->computed_value->to_dec_string();
        }
        
        if (i < enum_ref->members.size() - 1) {
            ss << ",";
        }
        ss << "\n";
    }
    
    ss << "}\n";
    return ss.str();
}

// Main entry point: generate all TypeScript code
std::string generate_typescript_output() {
    std::ostringstream output;
    
    // Track what imports we need
    bool need_address = false;
    bool need_cell = true;  // Always need Cell for Cell<T>
    bool need_dictionary = false;
    bool need_bitstring = false;
    
    // First pass: collect import requirements
    for (StructPtr struct_ref : G.all_structs) {
        // Skip generic structs (only generate instantiated versions)
        if (struct_ref->is_generic_struct()) continue;
        // Skip stdlib internal structs
        if (struct_ref->name.find("__") == 0) continue;
        
        collect_imports(struct_ref, need_address, need_cell, need_dictionary, need_bitstring);
    }
    
    // Generate imports header
    output << "/**\n";
    output << " * Auto-generated TypeScript bindings for Tolk structs.\n";
    output << " * Generated by Tolk compiler v" << "1.2" << "\n";  // TODO: use actual version
    output << " * \n";
    output << " * @ton/core types are used for compatibility with the TON ecosystem.\n";
    output << " */\n\n";
    
    output << "import { Builder, Slice";
    if (need_cell) output << ", Cell";
    if (need_address) output << ", Address, ExternalAddress";
    if (need_dictionary) output << ", Dictionary";
    if (need_bitstring) output << ", BitString";
    output << " } from '@ton/core';\n\n";
    
    // Generate enums first (structs may reference them)
    for (EnumDefPtr enum_ref : G.all_enums) {
        if (enum_ref->name.find("__") == 0) continue;
        
        output << "// === " << enum_ref->name << " ===\n";
        output << generate_enum(enum_ref);
        output << "\n";
    }
    
    // Generate structs
    for (StructPtr struct_ref : G.all_structs) {
        // Skip generic structs
        if (struct_ref->is_generic_struct()) continue;
        // Skip stdlib internal structs
        if (struct_ref->name.find("__") == 0) continue;
        // Skip instantiated generic structs with <T> in name (keep concrete ones like Wrapper<int>)
        // Actually, keep them - they're the ones we want
        
        output << "// === " << struct_ref->name << " ===\n";
        output << generate_opcode_constant(struct_ref);
        output << generate_struct_interface(struct_ref);
        output << "\n";
        output << generate_load_function(struct_ref);
        output << "\n";
        output << generate_store_function(struct_ref);
        output << "\n";
    }
    
    return output.str();
}

// Pipeline entry point
void pipeline_generate_ts_output() {
    if (!G_settings.emit_typescript) {
        return;
    }
    
    std::string ts_code = generate_typescript_output();
    
    // Always store in settings for WASM access
    G_settings.generated_typescript_code = ts_code;
    
    if (!G_settings.typescript_output_filename.empty()) {
        // -t<file>: Write TypeScript to specified file
        std::ofstream out(G_settings.typescript_output_filename);
        if (out.is_open()) {
            out << ts_code;
            out.close();
        } else {
            std::cerr << "Warning: could not write TypeScript output to " 
                      << G_settings.typescript_output_filename << std::endl;
        }
    } else {
        // -T flag: Output TypeScript to stdout after a separator
        // (Fift output already went to stdout or -o file)
        std::cout << "\n// ===== TYPESCRIPT OUTPUT =====\n\n";
        std::cout << ts_code;
    }
    // For WASM, the code is also available via G_settings.generated_typescript_code
}

}  // namespace tolk
