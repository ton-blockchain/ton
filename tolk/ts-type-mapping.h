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
#pragma once

#include <string>
#include <sstream>
#include "type-system.h"
#include "symtable.h"

namespace tolk {

/**
 * Maps Tolk types to TypeScript types and generates load/store expressions.
 * 
 * This module provides the core type mapping logic for TypeScript codegen.
 * It handles all Tolk types including:
 * - Primitive integers (intN, uintN, coins, varints)
 * - Address types (address, address?, any_address)
 * - Cell types (cell, Cell<T>, nullable variants)
 * - Compound types (structs, enums, unions)
 * - Special types (bitsN, bytesN, RemainingBitsAndRefs)
 */

struct TsTypeInfo {
    std::string ts_type;           // TypeScript type name (e.g., "bigint", "Address")
    std::string load_expr;         // Expression to load from slice (e.g., "slice.loadUintBig(64)")
    std::string store_expr;        // Expression to store to builder (e.g., "builder.storeUint(src.{field}, 64)")
    bool needs_import_address = false;
    bool needs_import_cell = false;
    bool needs_import_dictionary = false;
};

// Get TypeScript type info for a Tolk type
inline TsTypeInfo get_ts_type_info(TypePtr type, const std::string& field_name = "") {
    TsTypeInfo info;
    
    // Unwrap aliases
    type = type->unwrap_alias();
    
    // Handle intN types
    if (auto* int_n = type->try_as<TypeDataIntN>()) {
        int bits = int_n->n_bits;
        bool is_unsigned = int_n->is_unsigned;
        bool is_variadic = int_n->is_variadic;
        
        if (is_variadic) {
            // varint16, varint32, varuint16, varuint32
            info.ts_type = "bigint";
            if (is_unsigned) {
                info.load_expr = "slice.loadVarUintBig(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeVarUint(src." + field_name + ", " + std::to_string(bits) + ")";
            } else {
                info.load_expr = "slice.loadVarIntBig(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeVarInt(src." + field_name + ", " + std::to_string(bits) + ")";
            }
        } else if (bits <= 32) {
            // Small integers fit in JS number
            info.ts_type = "number";
            if (is_unsigned) {
                info.load_expr = "slice.loadUint(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeUint(src." + field_name + ", " + std::to_string(bits) + ")";
            } else {
                info.load_expr = "slice.loadInt(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeInt(src." + field_name + ", " + std::to_string(bits) + ")";
            }
        } else {
            // Large integers need bigint
            info.ts_type = "bigint";
            if (is_unsigned) {
                info.load_expr = "slice.loadUintBig(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeUint(src." + field_name + ", " + std::to_string(bits) + ")";
            } else {
                info.load_expr = "slice.loadIntBig(" + std::to_string(bits) + ")";
                info.store_expr = "builder.storeInt(src." + field_name + ", " + std::to_string(bits) + ")";
            }
        }
        return info;
    }
    
    // Handle coins type
    if (type->try_as<TypeDataCoins>()) {
        info.ts_type = "bigint";
        info.load_expr = "slice.loadCoins()";
        info.store_expr = "builder.storeCoins(src." + field_name + ")";
        return info;
    }
    
    // Handle bool type
    if (type->try_as<TypeDataBool>()) {
        info.ts_type = "boolean";
        info.load_expr = "slice.loadBoolean()";
        info.store_expr = "builder.storeBit(src." + field_name + ")";
        return info;
    }
    
    // Handle address type
    if (auto* addr = type->try_as<TypeDataAddress>()) {
        info.needs_import_address = true;
        if (addr->get_type_id() == TypeData::type_id_address_any) {
            info.ts_type = "Address | ExternalAddress | null";
            info.load_expr = "slice.loadAddressAny()";
            info.store_expr = "builder.storeAddress(src." + field_name + ")";
        } else {
            info.ts_type = "Address";
            info.load_expr = "slice.loadAddress()";
            info.store_expr = "builder.storeAddress(src." + field_name + ")";
        }
        return info;
    }
    
    // Handle cell type
    if (type->try_as<TypeDataCell>()) {
        info.ts_type = "Cell";
        info.needs_import_cell = true;
        info.load_expr = "slice.loadRef()";
        info.store_expr = "builder.storeRef(src." + field_name + ")";
        return info;
    }
    
    // Handle slice type
    if (type->try_as<TypeDataSlice>()) {
        info.ts_type = "Slice";
        info.load_expr = "slice.clone()";  // RemainingBitsAndRefs case
        info.store_expr = "builder.storeSlice(src." + field_name + ")";
        return info;
    }
    
    // Handle builder type
    if (type->try_as<TypeDataBuilder>()) {
        info.ts_type = "Builder";
        info.load_expr = "/* cannot load builder */";
        info.store_expr = "builder.storeBuilder(src." + field_name + ")";
        return info;
    }
    
    // Handle bitsN/bytesN types
    if (auto* bits_n = type->try_as<TypeDataBitsN>()) {
        info.ts_type = "Buffer";
        int width = bits_n->n_width;
        if (bits_n->is_bits) {
            // bitsN - width is in bits
            if (width % 8 == 0) {
                info.load_expr = "slice.loadBuffer(" + std::to_string(width / 8) + ")";
                info.store_expr = "builder.storeBuffer(src." + field_name + ")";
            } else {
                info.load_expr = "slice.loadBits(" + std::to_string(width) + ")";
                info.store_expr = "builder.storeBits(src." + field_name + ")";
            }
        } else {
            // bytesN - width is in bytes
            info.load_expr = "slice.loadBuffer(" + std::to_string(width) + ")";
            info.store_expr = "builder.storeBuffer(src." + field_name + ")";
        }
        return info;
    }
    
    // Handle struct types
    if (auto* struct_type = type->try_as<TypeDataStruct>()) {
        StructPtr struct_ref = struct_type->struct_ref;
        info.ts_type = struct_ref->name;
        info.load_expr = "load" + struct_ref->name + "(slice)";
        info.store_expr = "store" + struct_ref->name + "(src." + field_name + ")(builder)";
        return info;
    }
    
    // Handle enum types
    if (auto* enum_type = type->try_as<TypeDataEnum>()) {
        EnumDefPtr enum_ref = enum_type->enum_ref;
        info.ts_type = enum_ref->name;
        // Enums serialize as their backing integer type
        TypePtr colon_type = enum_ref->colon_type;
        if (colon_type) {
            TsTypeInfo backing = get_ts_type_info(colon_type, field_name);
            info.load_expr = backing.load_expr + " as " + enum_ref->name;
            info.store_expr = backing.store_expr;  // Store as number
        } else {
            // Default: uint32
            info.load_expr = "slice.loadUint(32) as " + enum_ref->name;
            info.store_expr = "builder.storeUint(src." + field_name + ", 32)";
        }
        return info;
    }
    
    // Handle union types (T1 | T2 | ...)
    if (auto* union_type = type->try_as<TypeDataUnion>()) {
        // Check for nullable (T | null)
        if (union_type->or_null != nullptr && union_type->size() == 2) {
            // This is T? (nullable)
            TsTypeInfo inner = get_ts_type_info(union_type->or_null, field_name);
            info.ts_type = inner.ts_type + " | null";
            info.needs_import_address = inner.needs_import_address;
            info.needs_import_cell = inner.needs_import_cell;
            info.needs_import_dictionary = inner.needs_import_dictionary;
            
            // For primitive nullables, the null is stored in the same slot
            if (union_type->is_primitive_nullable()) {
                // e.g., address? uses loadMaybeAddress
                if (union_type->or_null->try_as<TypeDataAddress>()) {
                    info.load_expr = "slice.loadMaybeAddress()";
                    info.store_expr = "builder.storeAddress(src." + field_name + ")";
                } else if (union_type->or_null->try_as<TypeDataCell>()) {
                    info.load_expr = "slice.loadMaybeRef()";
                    info.store_expr = "builder.storeMaybeRef(src." + field_name + ")";
                } else {
                    // Generic nullable: 1-bit flag + value
                    info.load_expr = "slice.loadBit() ? " + inner.load_expr + " : null";
                    info.store_expr = "src." + field_name + " !== null ? (builder.storeBit(true), " + inner.store_expr + ") : builder.storeBit(false)";
                }
            } else {
                // Complex nullable: value slots + type tag
                info.load_expr = "/* complex nullable: " + inner.ts_type + " | null */";
                info.store_expr = "/* complex nullable store */";
            }
            return info;
        }
        
        // Full union type - generate discriminated union
        std::ostringstream type_ss;
        for (size_t i = 0; i < union_type->variants.size(); i++) {
            if (i > 0) type_ss << " | ";
            TsTypeInfo variant = get_ts_type_info(union_type->variants[i]);
            type_ss << variant.ts_type;
            info.needs_import_address |= variant.needs_import_address;
            info.needs_import_cell |= variant.needs_import_cell;
            info.needs_import_dictionary |= variant.needs_import_dictionary;
        }
        info.ts_type = type_ss.str();
        info.load_expr = "/* union load - check opcodes */";
        info.store_expr = "/* union store - switch on $$type */";
        return info;
    }
    
    // Handle map<K, V>
    if (auto* map_type = type->try_as<TypeDataMapKV>()) {
        TsTypeInfo key_info = get_ts_type_info(map_type->TKey);
        TsTypeInfo val_info = get_ts_type_info(map_type->TValue);
        info.ts_type = "Dictionary<" + key_info.ts_type + ", " + val_info.ts_type + ">";
        info.needs_import_dictionary = true;
        info.load_expr = "Dictionary.load(/* key dict, value dict */)";
        info.store_expr = "src." + field_name + ".store(builder)";
        return info;
    }
    
    // Fallback for unknown types
    info.ts_type = "unknown /* " + type->as_human_readable() + " */";
    info.load_expr = "/* unknown type */";
    info.store_expr = "/* unknown type */";
    return info;
}

// Generate the full load function body for a struct
inline std::string generate_load_function_body(StructPtr struct_ref) {
    std::ostringstream ss;
    
    // Handle opcode if present
    if (struct_ref->opcode.exists()) {
        ss << "    if (slice.loadUint(" << struct_ref->opcode.prefix_len << ") !== 0x" 
           << std::hex << struct_ref->opcode.pack_prefix << std::dec << ") {\n";
        ss << "        throw new Error('Invalid opcode for " << struct_ref->name << "');\n";
        ss << "    }\n";
    }
    
    // Load each field
    for (int i = 0; i < struct_ref->get_num_fields(); i++) {
        StructFieldPtr field = struct_ref->get_field(i);
        if (field->is_private) continue;  // Skip private fields in generated API
        
        TypePtr field_type = field->declared_type;
        TsTypeInfo type_info = get_ts_type_info(field_type, field->name);
        
        ss << "    const " << field->name << " = " << type_info.load_expr << ";\n";
    }
    
    // Build return object
    ss << "    return {\n";
    if (struct_ref->opcode.exists()) {
        ss << "        $$type: '" << struct_ref->name << "',\n";
    }
    for (int i = 0; i < struct_ref->get_num_fields(); i++) {
        StructFieldPtr field = struct_ref->get_field(i);
        if (field->is_private) continue;
        ss << "        " << field->name << ",\n";
    }
    ss << "    };\n";
    
    return ss.str();
}

// Generate the full store function body for a struct
inline std::string generate_store_function_body(StructPtr struct_ref) {
    std::ostringstream ss;
    
    ss << "    return (builder: Builder) => {\n";
    
    // Handle opcode if present
    if (struct_ref->opcode.exists()) {
        ss << "        builder.storeUint(0x" << std::hex << struct_ref->opcode.pack_prefix 
           << std::dec << ", " << struct_ref->opcode.prefix_len << ");\n";
    }
    
    // Store each field
    for (int i = 0; i < struct_ref->get_num_fields(); i++) {
        StructFieldPtr field = struct_ref->get_field(i);
        if (field->is_private) continue;
        
        TypePtr field_type = field->declared_type;
        TsTypeInfo type_info = get_ts_type_info(field_type, field->name);
        
        ss << "        " << type_info.store_expr << ";\n";
    }
    
    ss << "    };\n";
    
    return ss.str();
}

}  // namespace tolk
