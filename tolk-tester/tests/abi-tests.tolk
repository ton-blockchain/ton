contract AbiTests {
    author: "Tolk team"
    version: "1.0"
    description: "ABI export regression test"
    storage: ContractStorage
    storageAtDeployment: InitStorage
    incomingMessages: Increment | Reset
    forceAbiExport: ExtraPayload
}

/// Persistent contract data
struct ContractStorage {
    /// Current counter value
    counter: int32
    /// Contract owner
    owner: address
}

/// Initial deployment data.
struct InitStorage {
    /// Contract owner at deployment
    owner: address
}

/// Extra payload exported on demand.
struct ExtraPayload {
    /// Human-readable note
    @abi.clientType(string)
    note: RemainingBitsAndRefs
}

/// Increases the counter.
struct (0x01) Increment {
    /// Amount to add
    by: int32
    queryId: uint64 = 0
}

/// Resets the counter.
struct (0x02) Reset {
    /// New counter value
    value: int32
}

/// Runs a tiny script.
/// Not onInternalMessage.
/// @param amount initial stack value
/// @param times added stack value
fun main(amount: int, times: int): int {
    return amount + times;
}

/// Reads current counter.
/// From storage.
get fun currentCounter(): int32 {
    val storage = lazy ContractStorage.fromCell(contract.getData());
    return storage.counter;
}

struct OneOrTwo<T1, T2> {
    uni: T1 | T2
}

type Box<T> = T | int32

struct Holder<T> {
    v: Box<T>
}

get fun multiple(): (Box<int64>, Holder<int32>, OneOrTwo<int32, int64>)? {
    return null
}

/**
@testcase | 0 | 7 5 | 12

@abi_json
"""
  "contract_name": "AbiTests",
  "author": "Tolk team",
  "version": "1.0",
  "description": "ABI export regression test",
"""

@abi_json
"""
    {
      "kind": "struct",
      "name": "ContractStorage",
      "ty_idx": 14,
      "fields": [
        {
          "name": "counter",
          "ty_idx": 8,
          "description": "Current counter value"
        },
        {
          "name": "owner",
          "ty_idx": 7,
          "description": "Contract owner"
        }
      ]
    },
"""

@abi_json
"""
    {
      "kind": "struct",
      "name": "ExtraPayload",
      "ty_idx": 16,
      "fields": [
        {
          "name": "note",
          "ty_idx": 18,
          "description": "Human-readable note"
        }
      ],
      "overrides_client_type": true
    },
"""

@abi_json
"""
    {
      "kind": "struct",
      "name": "Increment",
      "ty_idx": 12,
      "prefix": {
        "prefix_num": 1,
        "prefix_len": 8
      },
      "fields": [
        {
          "name": "by",
          "ty_idx": 8,
          "description": "Amount to add"
        },
        {
          "name": "queryId",
          "ty_idx": 11,
          "default_value": {
            "kind": "int",
            "v": "0"
          }
        }
      ]
    },
"""

@abi_json
"""
  "unique_types": [
    {"kind":"void"},
    {"kind":"int"},
"""

@abi_json
"""
  "storage": {
    "storage_ty_idx": 14,
    "storage_at_deployment_ty_idx": 15,
    "description": "Persistent contract data"
  },
"""

@abi_json
"""
  "incoming_messages": [
    {
      "body_ty_idx": 12,
      "description": "Increases the counter."
    },
    {
      "body_ty_idx": 13,
      "description": "Resets the counter."
    }
  ],
"""

@abi_json
"""
    {
      "tvm_method_id": 0,
      "name": "main",
      "parameters": [
        {
          "name": "amount",
          "ty_idx": 1,
          "description": "initial stack value"
        },
        {
          "name": "times",
          "ty_idx": 1,
          "description": "added stack value"
        }
      ],
      "return_ty_idx": 1,
      "description": "Runs a tiny script.\nNot onInternalMessage."
    },
"""

@abi_json
"""
    {
      "kind": "struct",
      "name": "OneOrTwo",
      "ty_idx": 32,
      "type_params": [
        "T1",
        "T2"
      ],
      "fields": [
        {
          "name": "uni",
          "ty_idx": 33
        }
      ]
    },
"""

@abi_json
"""
    {"kind":"StructRef","struct_name":"OneOrTwo","type_args_ty_idx":[8,10]},
"""

@abi_json
"""
    {"kind":"nullable","inner_ty_idx":34,"stack_type_id":131,"stack_width":6}
"""

@abi_json
"""
  "struct_instantiations": [
    {
      "ty_idx": 25,
      "struct_name": "Holder<int32>",
      "monomorphic_fields_ty_idx": [
        26
      ]
    },
    {
      "ty_idx": 28,
      "struct_name": "OneOrTwo<int32, int64>",
      "monomorphic_fields_ty_idx": [
        29
      ]
    }
  ],
  "alias_instantiations": [
    {
      "ty_idx": 20,
      "alias_name": "Box<int64>",
      "monomorphic_target_ty_idx": 21
    },
    {
      "ty_idx": 26,
      "alias_name": "Box<int32>",
      "monomorphic_target_ty_idx": 8
    }
  ],
"""
 */
