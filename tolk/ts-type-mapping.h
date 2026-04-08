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
#include <cmath>
#include <iomanip>
#include "type-system.h"
#include "symtable.h"
#include "pack-unpack-serializers.h"

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
    bool needs_import_bitstring = false;
};

// Forward declaration
inline TsTypeInfo get_ts_type_info(TypePtr type, const std::string& field_name = "");

// Generate Dictionary.Keys expression for a map key type
inline std::string get_dict_key_expr(TypePtr key_type) {
    key_type = key_type->unwrap_alias();
    if (auto* int_n = key_type->try_as<TypeDataIntN>()) {
        if (int_n->is_unsigned) {
            return "Dictionary.Keys.Uint(" + std::to_string(int_n->n_bits) + ")";
        } else {
            return "Dictionary.Keys.Int(" + std::to_string(int_n->n_bits) + ")";
        }
    }
    if (key_type->try_as<TypeDataInt>()) {
        return "Dictionary.Keys.BigInt(257)";
    }
    if (key_type->try_as<TypeDataAddress>()) {
        return "Dictionary.Keys.Address()";
    }
    if (auto* bits_n = key_type->try_as<TypeDataBitsN>()) {
        int width = bits_n->is_bits ? bits_n->n_width : bits_n->n_width * 8;
        return "Dictionary.Keys.Buffer(" + std::to_string(width / 8) + ")";
    }
    if (key_type->try_as<TypeDataBool>()) {
        return "Dictionary.Keys.Uint(1)";
    }
    // Struct-typed keys use their total bit width if known
    if (auto* struct_type = key_type->try_as<TypeDataStruct>()) {
        // Struct keys need custom key serialization
        StructPtr struct_ref = struct_type->struct_ref;
        return "/* struct key type: " + struct_ref->name + " — use custom Dictionary.Keys */";
    }
    return "/* unsupported key type: " + key_type->as_human_readable() + " */";
}

// Generate inline value serializer for dictionary values
inline std::string get_dict_value_expr(TypePtr val_type) {
    // For struct types, use load/store functions
    if (auto* struct_type = val_type->unwrap_alias()->try_as<TypeDataStruct>()) {
        StructPtr struct_ref = struct_type->struct_ref;
        return "{\n            serialize: (src, builder) => store" + struct_ref->name + "(src)(builder),\n"
               "            parse: (slice) => load" + struct_ref->name + "(slice),\n        }";
    }
    // For primitives, generate inline serializers using a placeholder field name
    TsTypeInfo val_info = get_ts_type_info(val_type, "DICT_VAL");
    std::string load_expr = val_info.load_expr;
    std::string store_expr = val_info.store_expr;
    // Replace "src.DICT_VAL" with "src" for inline serialization
    std::string needle = "src.DICT_VAL";
    size_t pos = 0;
    while ((pos = store_expr.find(needle, pos)) != std::string::npos) {
        store_expr.replace(pos, needle.size(), "src");
        pos += 3;
    }
    return "{\n            serialize: (src, builder) => { " + store_expr + "; },\n"
           "            parse: (slice) => " + load_expr + ",\n        }";
}

// Generate a load expression for a single union variant
inline std::string get_variant_load_expr(TypePtr variant_type) {
    variant_type = variant_type->unwrap_alias();
    if (auto* struct_type = variant_type->try_as<TypeDataStruct>()) {
        return "load" + struct_type->struct_ref->name + "(slice)";
    }
    // For primitive types, just use the standard load
    TsTypeInfo vi = get_ts_type_info(variant_type);
    return vi.load_expr;
}

// Generate a store expression for a single union variant, given source variable name
inline std::string get_variant_store_expr(TypePtr variant_type, const std::string& src_var) {
    variant_type = variant_type->unwrap_alias();
    if (auto* struct_type = variant_type->try_as<TypeDataStruct>()) {
        return "store" + struct_type->struct_ref->name + "(" + src_var + ")(builder)";
    }
    // For primitives, generate inline store
    TsTypeInfo vi = get_ts_type_info(variant_type, "VALUE_PLACEHOLDER");
    std::string expr = vi.store_expr;
    // Replace ALL occurrences of "src.VALUE_PLACEHOLDER" with the actual source
    std::string needle = "src.VALUE_PLACEHOLDER";
    size_t pos = 0;
    while ((pos = expr.find(needle, pos)) != std::string::npos) {
        expr.replace(pos, needle.size(), src_var);
        pos += src_var.size();
    }
    return expr;
}

// Generate TypeScript type guard condition for a variant
inline std::string get_variant_type_guard(TypePtr variant_type, const std::string& src_var) {
    variant_type = variant_type->unwrap_alias();
    if (auto* struct_type = variant_type->try_as<TypeDataStruct>()) {
        if (struct_type->struct_ref->opcode.exists()) {
            return src_var + ".$$type === '" + struct_type->struct_ref->name + "'";
        }
        // No opcode — check for a distinguishing field or use $$type if available
        return "'" + struct_type->struct_ref->name + "' in " + src_var;
    }
    // For primitive types
    if (variant_type->try_as<TypeDataInt>() || variant_type->try_as<TypeDataIntN>() || variant_type->try_as<TypeDataCoins>()) {
        return "typeof " + src_var + " === 'bigint' || typeof " + src_var + " === 'number'";
    }
    if (variant_type->try_as<TypeDataBool>()) {
        return "typeof " + src_var + " === 'boolean'";
    }
    if (variant_type->try_as<TypeDataAddress>()) {
        return src_var + " instanceof Address";
    }
    if (variant_type->try_as<TypeDataCell>()) {
        return src_var + " instanceof Cell";
    }
    if (variant_type->try_as<TypeDataSlice>()) {
        return src_var + " instanceof Slice";
    }
    if (variant_type->try_as<TypeDataBuilder>()) {
        return src_var + " instanceof Builder";
    }
    if (auto* bits_n = variant_type->try_as<TypeDataBitsN>()) {
        if (bits_n->is_bits && bits_n->n_width % 8 == 0) {
            return "Buffer.isBuffer(" + src_var + ") && " + src_var + ".length === " + std::to_string(bits_n->n_width / 8);
        } else if (!bits_n->is_bits) {
            return "Buffer.isBuffer(" + src_var + ") && " + src_var + ".length === " + std::to_string(bits_n->n_width);
        } else {
            return src_var + " instanceof BitString";
        }
    }
    if (auto* enum_type = variant_type->try_as<TypeDataEnum>()) {
        return "typeof " + src_var + " === 'number' /* " + enum_type->enum_ref->name + " */";
    }
    if (variant_type->try_as<TypeDataMapKV>()) {
        return src_var + " instanceof Dictionary";
    }
    return "true /* " + variant_type->as_human_readable() + " */";
}

// Generate union load expression using auto_generate_opcodes_for_union
inline std::string generate_union_load_expr(TypePtr union_type, const std::string& field_name) {
    const TypeDataUnion* t_union = union_type->try_as<TypeDataUnion>();
    if (!t_union) return "/* invalid union */";

    std::string err_msg;
    std::vector<PackOpcode> opcodes = auto_generate_opcodes_for_union(union_type, err_msg);
    
    if (opcodes.empty()) {
        return "/* union load failed: " + err_msg + " */";
    }
    
    // Check if all variants have manual opcodes (Case A)
    bool all_manual = true;
    bool has_null = false;
    for (TypePtr variant : t_union->variants) {
        if (variant == TypeDataNullLiteral::create()) {
            has_null = true;
            continue;
        }
        auto* st = variant->unwrap_alias()->try_as<TypeDataStruct>();
        if (!st || !st->struct_ref->opcode.exists()) {
            all_manual = false;
            break;
        }
    }
    
    if (all_manual && !has_null) {
        // Case A: All structs have manual opcodes — use preloadUint
        // Find max prefix_len
        int prefix_len = opcodes[0].prefix_len;
        
        std::ostringstream ss;
        ss << "(() => {\n";
        ss << "        const prefix = slice.preloadUint(" << prefix_len << ");\n";
        for (size_t i = 0; i < t_union->variants.size(); i++) {
            ss << "        if (prefix === 0x" << std::hex << opcodes[i].pack_prefix << std::dec << ") return " << get_variant_load_expr(t_union->variants[i]) << ";\n";
        }
        ss << "        throw new Error('Unknown union variant');\n";
        ss << "    })()";
        return ss.str();
    }
    
    // Case B: Auto-generated prefix tree
    // Determine which variants are null vs non-null
    std::ostringstream ss;
    ss << "(() => {\n";
    
    if (has_null) {
        // Null is encoded as single 0 bit
        ss << "        const hasValue = slice.loadBit();\n";
        ss << "        if (!hasValue) return null;\n";
        
        // Count non-null variants
        int n_non_null = 0;
        for (TypePtr variant : t_union->variants) {
            if (variant != TypeDataNullLiteral::create()) n_non_null++;
        }
        
        if (n_non_null == 1) {
            // Only one non-null variant, no need for tag
            for (TypePtr variant : t_union->variants) {
                if (variant != TypeDataNullLiteral::create()) {
                    ss << "        return " << get_variant_load_expr(variant) << ";\n";
                }
            }
        } else {
            int prefix_len = static_cast<int>(std::ceil(std::log2(n_non_null)));
            ss << "        const tag = slice.loadUint(" << prefix_len << ");\n";
            int cur = 0;
            for (TypePtr variant : t_union->variants) {
                if (variant == TypeDataNullLiteral::create()) continue;
                ss << "        if (tag === " << cur << ") return " << get_variant_load_expr(variant) << ";\n";
                cur++;
            }
            ss << "        throw new Error('Unknown union tag');\n";
        }
    } else {
        // No null — sequential prefix tags
        int n = t_union->size();
        int prefix_len = static_cast<int>(std::ceil(std::log2(n)));
        if (prefix_len == 0) prefix_len = 1;  // edge case for single variant
        
        ss << "        const tag = slice.loadUint(" << prefix_len << ");\n";
        for (int i = 0; i < n; i++) {
            ss << "        if (tag === " << i << ") return " << get_variant_load_expr(t_union->variants[i]) << ";\n";
        }
        ss << "        throw new Error('Unknown union tag');\n";
    }
    
    ss << "    })()";
    return ss.str();
}

// Generate union store expression
inline std::string generate_union_store_expr(TypePtr union_type, const std::string& field_name) {
    const TypeDataUnion* t_union = union_type->try_as<TypeDataUnion>();
    if (!t_union) return "/* invalid union */";

    std::string err_msg;
    std::vector<PackOpcode> opcodes = auto_generate_opcodes_for_union(union_type, err_msg);
    
    if (opcodes.empty()) {
        return "/* union store failed: " + err_msg + " */";
    }
    
    std::string src_var = "src." + field_name;
    
    // Check if all variants have manual opcodes (Case A)
    bool all_manual = true;
    bool has_null = false;
    for (TypePtr variant : t_union->variants) {
        if (variant == TypeDataNullLiteral::create()) {
            has_null = true;
            continue;
        }
        auto* st = variant->unwrap_alias()->try_as<TypeDataStruct>();
        if (!st || !st->struct_ref->opcode.exists()) {
            all_manual = false;
            break;
        }
    }
    
    std::ostringstream ss;
    
    if (all_manual && !has_null) {
        // Case A: structs with manual opcodes — dispatch on $$type
        ss << "(() => {\n";
        for (size_t i = 0; i < t_union->variants.size(); i++) {
            auto* st = t_union->variants[i]->unwrap_alias()->try_as<TypeDataStruct>();
            std::string name = st->struct_ref->name;
            if (i == 0) {
                ss << "            if (" << src_var << ".$$type === '" << name << "') store" << name << "(" << src_var << " as " << name << ")(builder);\n";
            } else {
                ss << "            else if (" << src_var << ".$$type === '" << name << "') store" << name << "(" << src_var << " as " << name << ")(builder);\n";
            }
        }
        ss << "            else throw new Error('Unknown union variant');\n";
        ss << "        })()";
    } else {
        // Case B: auto-generated prefix tree
        ss << "(() => {\n";
        
        if (has_null) {
            ss << "            if (" << src_var << " === null) { builder.storeBit(false); return; }\n";
            ss << "            builder.storeBit(true);\n";
            
            int n_non_null = 0;
            for (TypePtr variant : t_union->variants) {
                if (variant != TypeDataNullLiteral::create()) n_non_null++;
            }
            
            if (n_non_null == 1) {
                for (TypePtr variant : t_union->variants) {
                    if (variant != TypeDataNullLiteral::create()) {
                        ss << "            " << get_variant_store_expr(variant, src_var) << ";\n";
                    }
                }
            } else {
                int prefix_len = static_cast<int>(std::ceil(std::log2(n_non_null)));
                int cur = 0;
                for (TypePtr variant : t_union->variants) {
                    if (variant == TypeDataNullLiteral::create()) continue;
                    std::string cond = get_variant_type_guard(variant, src_var);
                    if (cur == 0) {
                        ss << "            if (" << cond << ") { builder.storeUint(" << cur << ", " << prefix_len << "); " << get_variant_store_expr(variant, src_var) << "; }\n";
                    } else {
                        ss << "            else if (" << cond << ") { builder.storeUint(" << cur << ", " << prefix_len << "); " << get_variant_store_expr(variant, src_var) << "; }\n";
                    }
                    cur++;
                }
                ss << "            else throw new Error('Unknown union variant');\n";
            }
        } else {
            int n = t_union->size();
            int prefix_len = static_cast<int>(std::ceil(std::log2(n)));
            if (prefix_len == 0) prefix_len = 1;
            
            for (int i = 0; i < n; i++) {
                TypePtr variant = t_union->variants[i];
                std::string cond = get_variant_type_guard(variant, src_var);
                if (i == 0) {
                    ss << "            if (" << cond << ") { builder.storeUint(" << i << ", " << prefix_len << "); " << get_variant_store_expr(variant, src_var) << "; }\n";
                } else {
                    ss << "            else if (" << cond << ") { builder.storeUint(" << i << ", " << prefix_len << "); " << get_variant_store_expr(variant, src_var) << "; }\n";
                }
            }
            ss << "            else throw new Error('Unknown union variant');\n";
        }
        
        ss << "        })()";
    }
    
    return ss.str();
}

// Get TypeScript type info for a Tolk type
inline TsTypeInfo get_ts_type_info(TypePtr type, const std::string& field_name) {
    TsTypeInfo info;
    
    // Check for type aliases with custom pack/unpack BEFORE unwrapping
    if (auto* t_alias = type->try_as<TypeDataAlias>()) {
        FunctionPtr f_pack = nullptr, f_unpack = nullptr;
        get_custom_pack_unpack_function(type, f_pack, f_unpack);
        if (f_pack || f_unpack) {
            TsTypeInfo underlying = get_ts_type_info(t_alias->underlying_type, field_name);
            info = underlying;
            std::string alias_name = t_alias->alias_ref->name;
            if (f_unpack) {
                info.load_expr = "/* Note: uses custom serialization (" + alias_name + ".unpackFromSlice) */ " + underlying.load_expr;
            }
            if (f_pack) {
                info.store_expr = "/* Note: uses custom serialization (" + alias_name + ".packToBuilder) */ " + underlying.store_expr;
            }
            return info;
        }
    }
    
    // Unwrap aliases
    type = type->unwrap_alias();
    
    // Handle plain int type (257-bit TVM integer)
    if (type->try_as<TypeDataInt>()) {
        info.ts_type = "bigint";
        info.load_expr = "slice.loadIntBig(257)";
        info.store_expr = "builder.storeInt(src." + field_name + ", 257)";
        return info;
    }
    
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
        if (addr->is_any()) {
            info.ts_type = "Address | ExternalAddress | null";
            info.load_expr = "slice.loadAddressAny()";
            info.store_expr = "builder.storeAddress(src." + field_name + ")";
        } else {
            // addr->is_internal() — standard address
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
        int width = bits_n->n_width;
        if (bits_n->is_bits) {
            // bitsN - width is in bits
            if (width % 8 == 0) {
                // Byte-aligned: use Buffer
                info.ts_type = "Buffer";
                info.load_expr = "slice.loadBuffer(" + std::to_string(width / 8) + ")";
                info.store_expr = "builder.storeBuffer(src." + field_name + ")";
            } else {
                // Non-byte-aligned: use BitString
                info.ts_type = "BitString";
                info.needs_import_bitstring = true;
                info.load_expr = "slice.loadBits(" + std::to_string(width) + ")";
                info.store_expr = "builder.storeBits(src." + field_name + ")";
            }
        } else {
            // bytesN - width is in bytes, always use Buffer
            info.ts_type = "Buffer";
            info.load_expr = "slice.loadBuffer(" + std::to_string(width) + ")";
            info.store_expr = "builder.storeBuffer(src." + field_name + ")";
        }
        return info;
    }
    
    // Handle struct types (including Cell<T> which is a generic struct instantiation)
    if (auto* struct_type = type->try_as<TypeDataStruct>()) {
        StructPtr struct_ref = struct_type->struct_ref;
        
        // Check if this is Cell<T> — a typed reference
        if (struct_ref->is_instantiation_of_generic_struct() && 
            struct_ref->base_struct_ref != nullptr && 
            struct_ref->base_struct_ref->name == "Cell") {
            info.ts_type = "Cell";
            info.needs_import_cell = true;
            info.load_expr = "slice.loadRef()";
            info.store_expr = "builder.storeRef(src." + field_name + ")";
            return info;
        }
        
        info.ts_type = struct_ref->name;
        info.load_expr = "load" + struct_ref->name + "(slice)";
        info.store_expr = "store" + struct_ref->name + "(src." + field_name + ")(builder)";
        return info;
    }
    
    // Handle enum types
    if (auto* enum_type = type->try_as<TypeDataEnum>()) {
        EnumDefPtr enum_ref = enum_type->enum_ref;
        info.ts_type = enum_ref->name;
        // Enums serialize using the calculated bit width from calculate_intN_to_serialize_enum
        TypePtr pack_type = calculate_intN_to_serialize_enum(enum_ref);
        TsTypeInfo backing = get_ts_type_info(pack_type, field_name);
        info.load_expr = backing.load_expr + " as " + enum_ref->name;
        info.store_expr = backing.store_expr;  // Store as number
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
                // Complex nullable: struct or compound type with width > 1
                // Uses 1-bit flag + full value load/store
                info.load_expr = "slice.loadBit() ? " + inner.load_expr + " : null";
                // For complex nullable store, we need multi-line but inline it
                info.store_expr = "src." + field_name + " !== null ? (builder.storeBit(true), " + inner.store_expr + ") : builder.storeBit(false)";
            }
            return info;
        }
        
        // Full union type - build type string and generate load/store
        std::ostringstream type_ss;
        for (size_t i = 0; i < union_type->variants.size(); i++) {
            if (i > 0) type_ss << " | ";
            TypePtr variant_type = union_type->variants[i];
            if (variant_type == TypeDataNullLiteral::create()) {
                type_ss << "null";
            } else {
                TsTypeInfo variant = get_ts_type_info(variant_type);
                type_ss << variant.ts_type;
                info.needs_import_address |= variant.needs_import_address;
                info.needs_import_cell |= variant.needs_import_cell;
                info.needs_import_dictionary |= variant.needs_import_dictionary;
            }
        }
        info.ts_type = type_ss.str();
        
        // Generate union load/store using opcode information
        info.load_expr = generate_union_load_expr(type, field_name);
        info.store_expr = generate_union_store_expr(type, field_name);
        return info;
    }
    
    // Handle map<K, V>
    if (auto* map_type = type->try_as<TypeDataMapKV>()) {
        TsTypeInfo key_info = get_ts_type_info(map_type->TKey);
        TsTypeInfo val_info = get_ts_type_info(map_type->TValue);
        info.ts_type = "Dictionary<" + key_info.ts_type + ", " + val_info.ts_type + ">";
        info.needs_import_dictionary = true;
        info.needs_import_address |= key_info.needs_import_address | val_info.needs_import_address;
        info.needs_import_cell |= key_info.needs_import_cell | val_info.needs_import_cell;
        
        std::string key_expr = get_dict_key_expr(map_type->TKey);
        std::string val_expr = get_dict_value_expr(map_type->TValue);
        info.load_expr = "slice.loadDict(" + key_expr + ", " + val_expr + ")";
        info.store_expr = "src." + field_name + ".store(builder)";
        return info;
    }
    
    // Handle tensor types (T1, T2, ...) — multi-value tuples on stack
    if (auto* tensor = type->try_as<TypeDataTensor>()) {
        std::ostringstream ts_type_ss, load_ss, store_ss;
        ts_type_ss << "[";
        for (int i = 0; i < tensor->size(); i++) {
            TsTypeInfo item_info = get_ts_type_info(tensor->items[i]);
            if (i > 0) ts_type_ss << ", ";
            ts_type_ss << item_info.ts_type;
            info.needs_import_address |= item_info.needs_import_address;
            info.needs_import_cell |= item_info.needs_import_cell;
            info.needs_import_dictionary |= item_info.needs_import_dictionary;
            info.needs_import_bitstring |= item_info.needs_import_bitstring;
        }
        ts_type_ss << "]";
        info.ts_type = ts_type_ss.str();
        
        // Generate sequential load: [load0, load1, ...]
        load_ss << "[";
        for (int i = 0; i < tensor->size(); i++) {
            TsTypeInfo item_info = get_ts_type_info(tensor->items[i]);
            if (i > 0) load_ss << ", ";
            load_ss << item_info.load_expr;
        }
        load_ss << "]";
        info.load_expr = load_ss.str();
        
        // Generate sequential store
        store_ss << "(() => { const t = src." << field_name << "; ";
        for (int i = 0; i < tensor->size(); i++) {
            TsTypeInfo item_info = get_ts_type_info(tensor->items[i], "TUPLE_ELEM");
            std::string se = item_info.store_expr;
            // Replace ALL occurrences of "src.TUPLE_ELEM" with "t[i]"
            std::string needle = "src.TUPLE_ELEM";
            std::string replacement = "t[" + std::to_string(i) + "]";
            size_t pos = 0;
            while ((pos = se.find(needle, pos)) != std::string::npos) {
                se.replace(pos, needle.size(), replacement);
                pos += replacement.size();
            }
            store_ss << se << "; ";
        }
        store_ss << "})()";
        info.store_expr = store_ss.str();
        return info;
    }
    
    // Handle void type
    if (type->try_as<TypeDataVoid>()) {
        info.ts_type = "void";
        info.load_expr = "undefined";
        info.store_expr = "/* void - no serialization */";
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
        if (field->is_private) continue;
        
        TypePtr field_type = field->declared_type;
        // Skip void fields
        if (field_type->unwrap_alias()->try_as<TypeDataVoid>()) continue;
        
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
        if (field->declared_type->unwrap_alias()->try_as<TypeDataVoid>()) continue;
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
        // Skip void fields
        if (field_type->unwrap_alias()->try_as<TypeDataVoid>()) continue;
        
        TsTypeInfo type_info = get_ts_type_info(field_type, field->name);
        
        ss << "        " << type_info.store_expr << ";\n";
    }
    
    ss << "    };\n";
    
    return ss.str();
}

}  // namespace tolk
