import hashlib
import json
import time
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Never, cast, override

import nacl.signing
from bitarray import bitarray
from block.generated import (
    AccountType,
    Anon_4,
    Anon_10,
    ConfigParam,
    ConfigParams,
    DepthBalanceAug,
    KeyExtBlkRef,
    KeyMaxLt,
    KeyMaxLtAug,
    LibDescr,
    NewConsensusConfig,
    NewConsensusConfigType,
    OldMcBlocksInfo,
    OutMsgQueue,
    OutMsgQueueInfo,
    ProcessedInfo,
    ShardAccounts,
    ShardHashes,
    StateInit,
    StoragePrices,
    ValidatorDescr,
    ValidatorDescrType,
    account,
    account_active,
    account_descr,
    account_storage,
    addr_std,
    block_grams_created,
    block_limits,
    capabilities,
    catchain_config_new,
    cfg_vote_cfg,
    cfg_vote_setup,
    complaint_prices,
    consensus_config_v4,
    currencies,
    depth_balance,
    ed25519_pubkey,
    extra_currencies,
    gas_flat_pfx,
    gas_prices_ext,
    masterchain_state_extra,
    msg_forward_prices,
    nanograms,
    new_consensus_config_all,
    param_limits,
    shard_ident,
    shard_state,
    shared_lib_descr,
    simplex_config_v2,
    storage_extra_none,
    storage_info,
    storage_used,
    validator,
    validator_info,
    validators_ext,
    wc_split_merge_timings,
    wfmt_basic,
    workchain_v2,
)
from contract import (
    ConfigBlueprint,
    ContractBlueprint,
    ElectorBlueprint,
    Provider,
    WalletV1,
    WalletV1Blueprint,
    ton,
)
from pytoniq_core import Address, Builder, Cell, CurrencyCollection
from pytoniq_core import StateInit as PyStateInit
from tlb.hashmap import HashmapDict
from tlb.object import (
    CellRefType,
    Ref,
    UnitTypeInfo,
    VarUIntTypeConstructor,
    ref,
)
from tonapi import ton_api

from tl import JSONSerializable

from .key import Key
from .validator_set import compute_validator_set_hash


def _bits256(data: bytes) -> bitarray:
    ba = bitarray()
    ba.frombytes(data)
    assert len(ba) == 256
    return ba


def _bits256_int(value: int) -> bitarray:
    return _bits256(value.to_bytes(32, "big"))


def _zero_grams():
    return nanograms(amount=0)


def _grams(amount: int):
    return nanograms(amount=amount)


def _zero_cc():
    return currencies(
        grams=_zero_grams(),
        other=extra_currencies({}),
    )


def _cc(grams: int):
    return currencies(
        grams=_grams(grams),
        other=extra_currencies({}),
    )


GRAM = 1_000_000_000

# AllOnes = (2^256 - 1) / 15 = 0x1111...1111
_ALL_ONES = ((1 << 256) - 1) // 15

# Global capability bits (ton/ton-types.h GlobalCapabilities).
_CAP_CREATE_STATS = 2
_CAP_BOUNCE_MSG_BODY = 4
_CAP_REPORT_VERSION = 8
_CAP_SHORT_DEQUEUE = 32
_CAP_STORE_OUT_MSG_QUEUE_SIZE = 64
_CAP_MSG_METADATA = 128
_CAP_DEFER_MESSAGES = 256
_CAP_FULL_COLLATED_DATA = 512


@dataclass
class SimplexConsensusConfig:
    target_block_rate_ms: int = 400
    slots_per_leader_window: int = 4
    first_block_timeout_ms: int = 700
    use_quic: bool = True
    protocol_version: int = 2


@dataclass
class NetworkConfig:
    monitor_min_split: int = 0
    split: int = 0
    global_version: int = 14
    # Include capFullCollatedData in param 8 so shard-block validation runs
    # purely from collated-data proofs (no celldb state reads); mainnet parity.
    full_collated_data: bool = True
    shard_validators: int = 1
    block_limit_mul: int = 1
    gas_limit_mul: int = 1
    mc_valgroup_lifetime: int = 250
    mc_consensus: SimplexConsensusConfig | None = field(
        default_factory=lambda: SimplexConsensusConfig()
    )
    shard_valgroup_lifetime: int = 250
    shard_consensus: SimplexConsensusConfig | None = field(
        default_factory=lambda: SimplexConsensusConfig()
    )


@dataclass
class WorkchainState:
    file: Path | None
    file_hash: bytes
    root_hash: bytes


@dataclass(frozen=True)
class ExternalBasechainState:
    """Workchain-0 zerostate generated outside of tontester (e.g. by bench-state-gen).

    Only the hashes and aggregate values are known; the state's cells are expected
    to be pre-placed into the node's celldb, so no static BoC file is produced.
    """

    root_hash: bytes
    file_hash: bytes
    total_balance: int
    gen_utime: int

    @classmethod
    def from_manifest(cls, path: Path) -> "ExternalBasechainState":
        """Parse a bench-state-gen manifest.json (see benchmark/DESIGN.md)."""
        manifest = cast(JSONSerializable, json.loads(path.read_text()))
        assert isinstance(manifest, Mapping), f"{path}: manifest must be a JSON object"

        def get_str(key: str) -> str:
            value = manifest[key]
            assert isinstance(value, str), f"{path}: field {key!r} must be a string"
            return value

        def get_int(key: str) -> int:
            value = manifest[key]
            assert isinstance(value, int), f"{path}: field {key!r} must be an integer"
            return value

        root_hash = bytes.fromhex(get_str("root_hash_hex"))
        file_hash = bytes.fromhex(get_str("file_hash_hex"))
        assert len(root_hash) == 32 and len(file_hash) == 32
        return cls(
            root_hash=root_hash,
            file_hash=file_hash,
            total_balance=int(get_str("total_balance")),
            gen_utime=get_int("gen_utime"),
        )


# ---------------------------------------------------------------------------
# Zerostate builder — implements the contract.Zerostate protocol
# ---------------------------------------------------------------------------


@dataclass
class _SmcEntry:
    blueprint: ContractBlueprint[object]
    balance: int

    @property
    def address(self) -> int:
        return int.from_bytes(self.blueprint.address.hash_part, "big")

    @property
    def state_init_cell(self) -> Cell:
        return self.blueprint.state_init.serialize()

    @property
    def is_special(self) -> bool:
        return self.blueprint.state_init.special is not None


@dataclass(eq=False)
class ZerostateBuilder:
    """Accumulates smart contracts and produces the final zerostate BOC."""

    smcs: list[_SmcEntry] = field(default_factory=list)

    def deploy(self, smc: ContractBlueprint[object], balance: CurrencyCollection):
        self.smcs.append(_SmcEntry(blueprint=smc, balance=balance.grams))


# ---------------------------------------------------------------------------
# Account building from state_init
# ---------------------------------------------------------------------------


def _count_cells_and_bits(cell: Cell) -> tuple[int, int]:
    """Count total cells and bits in a cell tree (for StorageUsed)."""
    visited: set[bytes] = set()
    total_cells = 0
    total_bits = 0
    stack = [cell]
    while stack:
        c = stack.pop()
        h = c.hash
        if h in visited:
            continue
        visited.add(h)
        cs = c.begin_parse()
        total_cells += 1
        total_bits += cs.remaining_bits
        for _ in range(cs.remaining_refs):
            stack.append(cs.load_ref())
    return total_cells, total_bits


def _build_account(smc: _SmcEntry, workchain_id: int) -> account:
    state = StateInit.load_from(smc.state_init_cell.begin_parse())
    acc_storage = account_storage(
        last_trans_lt=0,
        balance=_cc(smc.balance),
        state=account_active(field=state),
    )
    storage_cell = acc_storage.serialize()
    cells, bits = _count_cells_and_bits(storage_cell)

    state2 = StateInit.load_from(smc.state_init_cell.begin_parse())
    return account(
        addr=addr_std(
            anycast=None,
            workchain_id=workchain_id,
            address=_bits256_int(smc.address),
        ),
        storage_stat=storage_info(
            used=storage_used(cells=cells, bits=bits),
            storage_extra=storage_extra_none(),
            last_paid=0,
            due_payment=None,
        ),
        storage=account_storage(
            last_trans_lt=0,
            balance=_cc(smc.balance),
            state=account_active(field=state2),
        ),
    )


def _build_shard_accounts(smcs: list[_SmcEntry], workchain_id: int) -> ShardAccounts:
    d = HashmapDict(
        256,
        account_descr,
        depth_balance,
        DepthBalanceAug(),
    )
    for smc in smcs:
        sa = account_descr(
            account=Ref(AccountType(), _build_account(smc, workchain_id)),
            last_trans_hash=_bits256(b"\x00" * 32),
            last_trans_lt=0,
        )
        d[smc.address] = sa
    return ShardAccounts(field=d)


# ---------------------------------------------------------------------------
# Config parameters
# ---------------------------------------------------------------------------


def _build_config_params(
    config: NetworkConfig,
    validator_keys: list[Key],
    now_time: int,
    wallet_addr: int,
) -> list[ConfigParam]:
    """Build the list of ConfigParam objects for the zerostate."""
    from block.generated import (
        ConfigParam_0,
        ConfigParam_1,
        ConfigParam_2,
        ConfigParam_7,
        ConfigParam_8,
        ConfigParam_9,
        ConfigParam_10,
        ConfigParam_11,
        ConfigParam_13,
        ConfigParam_14,
        ConfigParam_15,
        ConfigParam_16,
        ConfigParam_17,
        ConfigParam_18,
        ConfigParam_28,
        ConfigParam_29,
        ConfigParam_30,
        ConfigParam_34,
        config_block_limits,
        config_fwd_prices,
        config_gas_prices,
        config_mc_block_limits,
        config_mc_fwd_prices,
        config_mc_gas_prices,
    )

    params: list[ConfigParam] = []

    # Param 0: config address (AllOnes * 5)
    params.append(ConfigParam_0(config_addr=_bits256_int(_ALL_ONES * 5)))

    # Param 1: elector address (AllOnes * 3)
    params.append(ConfigParam_1(elector_addr=_bits256_int(_ALL_ONES * 3)))

    # Param 2: minter address (wallet)
    params.append(ConfigParam_2(minter_addr=_bits256_int(wallet_addr)))

    # Param 7: to_mint extra currencies
    mint_dict: HashmapDict[int] = HashmapDict(32, VarUIntTypeConstructor(32))
    mint_dict[0xFFFF_FFEF] = 1_000_000_000_000  # -17 as uint32
    mint_dict[239] = 666_666_666_666
    params.append(ConfigParam_7(to_mint=extra_currencies(dict=mint_dict)))

    # Param 8: version + capabilities.
    # Mainnet parity (fetched 2026-06-12): version=14, capabilities=0x3EE.
    cap_value = (
        _CAP_CREATE_STATS
        | _CAP_BOUNCE_MSG_BODY
        | _CAP_REPORT_VERSION
        | _CAP_SHORT_DEQUEUE
        | _CAP_STORE_OUT_MSG_QUEUE_SIZE
        | _CAP_MSG_METADATA
        | _CAP_DEFER_MESSAGES
    )
    if config.full_collated_data:
        cap_value |= _CAP_FULL_COLLATED_DATA
    params.append(
        ConfigParam_8(field=capabilities(version=config.global_version, capabilities=cap_value))
    )

    # Param 9: mandatory params
    mandatory = [0, 1, 9, 10, 12, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 28, 34]
    mandatory_dict: HashmapDict[None] = HashmapDict(32, UnitTypeInfo, allow_empty=False)
    for idx in mandatory:
        mandatory_dict[idx] = None
    params.append(ConfigParam_9(mandatory_params=mandatory_dict))

    # Param 10: critical params
    critical = [-999, -1000, -1001, 0, 1, 9, 10, 12, 14, 15, 16, 17, 32, 34, 36]
    critical_dict: HashmapDict[None] = HashmapDict(32, UnitTypeInfo, allow_empty=False)
    for idx in critical:
        critical_dict[idx & 0xFFFF_FFFF] = None
    params.append(ConfigParam_10(critical_params=critical_dict))

    # Param 11: config proposal setup
    normal_setup = cfg_vote_cfg(
        min_tot_rounds=2,
        max_tot_rounds=3,
        min_wins=2,
        max_losses=2,
        min_store_sec=1_000_000,
        max_store_sec=10_000_000,
        bit_price=1,
        cell_price=500,
    )
    critical_setup = cfg_vote_cfg(
        min_tot_rounds=4,
        max_tot_rounds=7,
        min_wins=4,
        max_losses=2,
        min_store_sec=5_000_000,
        max_store_sec=20_000_000,
        bit_price=2,
        cell_price=1_000,
    )
    params.append(
        ConfigParam_11(
            field=cfg_vote_setup(
                normal_params=ref(normal_setup),
                critical_params=ref(critical_setup),
            )
        )
    )

    # Param 13: complaint prices
    params.append(
        ConfigParam_13(
            field=complaint_prices(
                deposit=_grams(100 * GRAM),
                bit_price=_grams(1),
                cell_price=_grams(500),
            )
        )
    )

    # Param 14: block create fees
    params.append(
        ConfigParam_14(
            field=block_grams_created(
                masterchain_block_fee=_grams(int(1.7 * GRAM)),
                basechain_block_fee=_grams(1 * GRAM),
            )
        )
    )

    # Param 15: election params
    params.append(
        ConfigParam_15(
            validators_elected_for=2400,
            elections_start_before=800,
            elections_end_before=60,
            stake_held_for=300,
        )
    )

    # Param 16: validator counts
    params.append(
        ConfigParam_16(
            max_validators=1000,
            max_main_validators=1000,
            min_validators=1000,
        )
    )

    # Param 17: stake limits
    params.append(
        ConfigParam_17(
            min_stake=_grams(10_000 * GRAM),
            max_stake=_grams(100_000 * GRAM),
            min_total_stake=_grams(10_000 * GRAM),
            max_stake_factor=10 << 16,
        )
    )

    # Param 18: storage prices
    sp = StoragePrices(
        utime_since=0,
        bit_price_ps=1,
        cell_price_ps=500,
        mc_bit_price_ps=1000,
        mc_cell_price_ps=500_000,
    )
    sp_dict: HashmapDict[StoragePrices] = HashmapDict(32, StoragePrices, allow_empty=False)
    sp_dict[0] = sp
    params.append(ConfigParam_18(field=sp_dict))

    # Param 20: mc gas prices
    gas_mul = config.gas_limit_mul
    params.append(
        config_mc_gas_prices(
            field=gas_flat_pfx(
                flat_gas_limit=100,
                flat_gas_price=1000,
                other=gas_prices_ext(
                    gas_price=10 << 16,
                    gas_limit=1_000_000,
                    special_gas_limit=20_000_000,
                    gas_credit=10_000,
                    block_gas_limit=1000 * 1_000_000 * gas_mul,
                    freeze_due_limit=int(0.1 * GRAM),
                    delete_due_limit=1 * GRAM,
                ),
            )
        )
    )

    # Param 21: gas prices
    params.append(
        config_gas_prices(
            field=gas_flat_pfx(
                flat_gas_limit=100,
                flat_gas_price=1000,
                other=gas_prices_ext(
                    gas_price=10 << 16,
                    gas_limit=1_000_000,
                    special_gas_limit=1_000_000,
                    gas_credit=10_000,
                    block_gas_limit=1000 * 1_000_000 * gas_mul,
                    freeze_due_limit=int(0.1 * GRAM),
                    delete_due_limit=1 * GRAM,
                ),
            )
        )
    )

    # Params 22/23: block limits. Baselines are the CURRENT mainnet values
    # (fetched 2026-06-12 via toncenter getConfigParam); block_limit_mul scales
    # the bytes/lt soft+hard limits and gas_limit_mul the gas soft+hard limits.
    mul = config.block_limit_mul

    # Param 22: mc block limits (mainnet: bytes 128K/512K/1M, gas 200K/1M/2.5M,
    # lt 1000/5000/10000)
    params.append(
        config_mc_block_limits(
            field=block_limits(
                bytes=param_limits(
                    underload=131_072, soft_limit=524_288 * mul, hard_limit=1_048_576 * mul
                ),
                gas=param_limits(
                    underload=200_000,
                    soft_limit=1_000_000 * gas_mul,
                    hard_limit=2_500_000 * gas_mul,
                ),
                lt_delta=param_limits(
                    underload=1000, soft_limit=5000 * mul, hard_limit=10_000 * mul
                ),
            )
        )
    )

    # Param 23: block limits (mainnet: bytes 256K/1M/2M, gas 2M/10M/20M,
    # lt 1000/5000/10000)
    params.append(
        config_block_limits(
            field=block_limits(
                bytes=param_limits(
                    underload=262_144, soft_limit=1_048_576 * mul, hard_limit=2_097_152 * mul
                ),
                gas=param_limits(
                    underload=2_000_000,
                    soft_limit=10_000_000 * gas_mul,
                    hard_limit=20_000_000 * gas_mul,
                ),
                lt_delta=param_limits(
                    underload=1000, soft_limit=5000 * mul, hard_limit=10_000 * mul
                ),
            )
        )
    )

    # Param 24: mc fwd prices
    params.append(
        config_mc_fwd_prices(
            field=msg_forward_prices(
                lump_price=100,
                bit_price=10 << 16,
                cell_price=10 << 16,
                ihr_price_factor=int(1.5 * (1 << 16)),
                first_frac=int((1 / 3) * (1 << 16)),
                next_frac=int((1 / 3) * (1 << 16)),
            )
        )
    )

    # Param 25: fwd prices
    params.append(
        config_fwd_prices(
            field=msg_forward_prices(
                lump_price=100,
                bit_price=10 << 16,
                cell_price=10 << 16,
                ihr_price_factor=int(1.5 * (1 << 16)),
                first_frac=int((1 / 3) * (1 << 16)),
                next_frac=int((1 / 3) * (1 << 16)),
            )
        )
    )

    # Param 28: catchain config
    params.append(
        ConfigParam_28(
            field=catchain_config_new(
                flags=0,
                shuffle_mc_validators=True,
                mc_catchain_lifetime=config.mc_valgroup_lifetime,
                shard_catchain_lifetime=config.shard_valgroup_lifetime,
                shard_validators_lifetime=1000,
                shard_validators_num=config.shard_validators,
            )
        )
    )

    # Param 29: consensus config
    params.append(
        ConfigParam_29(
            field=consensus_config_v4(
                flags=0,
                use_quic=False,
                new_catchain_ids=True,
                round_candidates=3,
                next_candidate_delay_ms=2000,
                consensus_timeout_ms=16000,
                fast_attempts=3,
                attempt_duration=8,
                catchain_max_deps=4,
                max_block_bytes=2097152 * mul,
                max_collated_bytes=10485760 * mul,
                proto_version=5,
                catchain_max_blocks_coeff=10000,
            )
        )
    )

    # Param 30: new consensus params
    mc_ncp: NewConsensusConfig | None = None
    shard_ncp: NewConsensusConfig | None = None

    def convert_simplex_config(config: SimplexConsensusConfig) -> simplex_config_v2:
        return simplex_config_v2(
            flags=0,
            protocol_version=config.protocol_version,
            use_quic=config.use_quic,
            slots_per_leader_window=config.slots_per_leader_window,
            noncritical_params={
                0: config.target_block_rate_ms,
                1: config.first_block_timeout_ms,
            },
        )

    if isinstance(config.mc_consensus, SimplexConsensusConfig):
        mc_ncp = convert_simplex_config(config.mc_consensus)
    if isinstance(config.shard_consensus, SimplexConsensusConfig):
        shard_ncp = convert_simplex_config(config.shard_consensus)
    params.append(
        ConfigParam_30(
            field=new_consensus_config_all(
                mc=Ref(NewConsensusConfigType(), mc_ncp) if mc_ncp is not None else None,
                shard=Ref(NewConsensusConfigType(), shard_ncp) if shard_ncp is not None else None,
            )
        )
    )

    # Param 34: current validators
    val_list: HashmapDict[ValidatorDescr] = HashmapDict(16, ValidatorDescrType())
    total_weight = 0
    for i, key in enumerate(validator_keys):
        weight = 17
        total_weight += weight
        val = validator(
            public_key=ed25519_pubkey(pubkey=_bits256(key.public_key.key)),
            weight=weight,
        )
        val_list[i] = val

    n_validators = len(validator_keys)
    params.append(
        ConfigParam_34(
            cur_validators=validators_ext(
                utime_since=now_time,
                utime_until=now_time + 3600,
                total=n_validators,
                main=n_validators,
                total_weight=total_weight,
                list=val_list,
            )
        )
    )

    return params


# ---------------------------------------------------------------------------
# Empty container helpers
# ---------------------------------------------------------------------------


def _collect_public_libraries(smcs: list[_SmcEntry]) -> HashmapDict[LibDescr]:
    """Collect public libraries from all registered contracts, matching C++ store_public_libraries."""
    from block.generated import shared_lib_descr

    # Map: lib_hash -> (lib_cell, set of publisher addresses)
    libs: dict[bytes, tuple[Cell, set[int]]] = {}

    for smc in smcs:
        si = StateInit.load_from(smc.state_init_cell.begin_parse())
        if si.library is None:
            continue
        # Library cell is a Hashmap 256 SimpleLib (non-empty, not HashmapE)
        from block.generated import SimpleLib, simple_lib

        lib_dict = HashmapDict[SimpleLib].load_from(
            si.library.begin_parse(),
            256,
            simple_lib,
            allow_empty=False,
        )
        for lib_hash_int in lib_dict.keys():
            lib_entry = lib_dict[lib_hash_int]
            if not lib_entry.public:
                continue
            lib_cell: Cell = lib_entry.root
            h: bytes = lib_cell.hash
            if h not in libs:
                libs[h] = (lib_cell, set())
            libs[h][1].add(smc.address)

    result: HashmapDict[LibDescr] = HashmapDict(256, shared_lib_descr)
    for h, (lib_cell, publishers) in libs.items():
        # publishers is a Hashmap 256 True (non-empty)
        pub_dict: HashmapDict[None] = HashmapDict(256, UnitTypeInfo, allow_empty=False)
        for pub_addr in publishers:
            pub_dict[pub_addr] = None
        lib_descr = shared_lib_descr(lib=lib_cell, publishers=pub_dict)
        result[int.from_bytes(h, "big")] = lib_descr

    return result


# ---------------------------------------------------------------------------
# SMC#3 (test tick-tock contract) — built manually, no blueprint
# ---------------------------------------------------------------------------

# Compiled from Fift assembler — regenerate with extract_contract_bocs.py
SMC3_CODE = Cell.one_from_boc(
    "B5EE9C72410104010087000114FF00F4A413F4BCF2C80B0102012002030002D200DFA5FFFF76A268698FE9FFE8E42C5267858F90E785FFE4F6AA6467C444FFB365FFC10802FAF0807D014035E7A064B87D804077E7857FC10803DFD2407D014035E7A064B86467CD8903A32B9BA4410803ADE68AFD014035E7A045EA432B6363796103BB7B9363210C678B64B87D807D804097FA0370"
)
SMC3_LIBRARY = Cell.one_from_boc(
    "B5EE9C724101060100600002016201020142BF5A2EEF5056775F5B9572FF3AD63DD2A71D1FB281CA177A5E1C74730ECCB2E513030142BF412429205EA66D6F2004EDFA570F6F56B3E85E59BAA1BEFBC73B7DA5D55BDC6104000FABACABADABACABA80104123405000456789B1D76B1"
)
WALLET_LIBRARY = Cell.one_from_boc(
    "B5EE9C724101060100600002016201020142BF5A2EEF5056775F5B9572FF3AD63DD2A71D1FB281CA177A5E1C74730ECCB2E513030142BF412429205EA66D6F2004EDFA570F6F56B3E85E59BAA1BEFBC73B7DA5D55BDC6004000FABACABADABACABA801041234050004567876607CBC"
)


def _register_smc3(zs: ZerostateBuilder, wallet_addr: int) -> Address:
    """Register SMC#3 (tick-tock test contract) directly into the zerostate."""
    assert SMC3_CODE is not None and SMC3_LIBRARY is not None
    data = Builder().store_uint(0x11EF55AA, 32).store_uint(wallet_addr, 256).end_cell()
    from pytoniq_core.tlb.account import TickTock

    si = PyStateInit(
        special=TickTock(tick=True, tock=True),
        code=SMC3_CODE,
        data=data,
        library=SMC3_LIBRARY,
    )
    addr = Address((-1, si.serialize().hash))
    zs.smcs.append(_SmcEntry(blueprint=_RawBlueprint(si, addr), balance=ton(1).grams))
    return addr


@dataclass
class _RawBlueprint(ContractBlueprint[Never]):
    """Minimal ContractBlueprint for contracts not backed by a Blueprint subclass."""

    state_init: PyStateInit
    address: Address

    @override
    def materialize(self, provider: Provider) -> Never:
        raise NotImplementedError


# ---------------------------------------------------------------------------
# Zerostate result
# ---------------------------------------------------------------------------


@dataclass
class Zerostate:
    masterchain: WorkchainState
    shardchain: WorkchainState
    _wallet_bp: WalletV1Blueprint
    _elector_bp: ElectorBlueprint
    _config_bp: ConfigBlueprint

    def as_block(self):
        return ton_api.TonNode_blockIdExt(
            workchain=-1,
            shard=0x8000_0000_0000_0000 - (1 << 64),
            seqno=0,
            root_hash=self.masterchain.root_hash,
            file_hash=self.masterchain.file_hash,
        )

    def as_validator_config(self):
        return ton_api.Validator_config_global(zero_state=self.as_block())

    def main_wallet(self, provider: Provider) -> WalletV1:
        return self._wallet_bp.materialize(provider)


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def create_zerostate(
    state_dir: Path,
    config: NetworkConfig,
    validator_keys: list[Key],
    *,
    external_basechain: ExternalBasechainState | None = None,
) -> Zerostate:
    now_time = int(time.time())
    zs = ZerostateBuilder()

    # --- Wallet ---
    wallet_key = nacl.signing.SigningKey.generate()

    wallet_bp = WalletV1Blueprint(workchain=-1, private_key=wallet_key)
    wallet_bp.address = Address((-1, b"\x00" * 32))
    # Add test library to wallet state_init (for Fift parity — will be removed later)
    wallet_bp.state_init.library = WALLET_LIBRARY
    zs.deploy(wallet_bp, ton(4_999_990_000))

    # --- SMC#3 (test tick-tock) ---
    smc3_addr: Address | None = _register_smc3(zs, 0)  # wallet addr is AllOnes*0 = 0

    # --- Elector ---
    elector_bp = ElectorBlueprint()
    zs.deploy(elector_bp, ton(10))

    # --- Config params (needs wallet addr for minter) ---
    wallet_addr_int = 0  # AllOnes * 0
    config_params = _build_config_params(config, validator_keys, now_time, wallet_addr_int)

    # --- Build workchain 0 (base chain) empty shard state, or take an external one ---
    external_balance = 0
    if external_basechain is None:
        base_state = shard_state(
            global_id=-777,
            shard_id=shard_ident(shard_pfx_bits=0, workchain_id=0, shard_prefix=1 << 63),
            seq_no=0,
            vert_seq_no=0,
            gen_utime=now_time,
            gen_lt=0,
            min_ref_mc_seqno=0xFFFF_FFFF,
            out_msg_queue_info=ref(
                OutMsgQueueInfo(
                    out_queue=OutMsgQueue({}),
                    proc_info=ProcessedInfo({}),
                    extra=None,
                )
            ),
            before_split=0,
            accounts=ref(
                ShardAccounts(
                    field=HashmapDict(256, account_descr, depth_balance, DepthBalanceAug()),
                )
            ),
            field=ref(
                Anon_4(
                    overload_history=0,
                    underload_history=0,
                    total_balance=_zero_cc(),
                    total_validator_fees=_zero_cc(),
                    libraries=HashmapDict(256, shared_lib_descr),
                    master_ref=None,
                )
            ),
            custom=None,
        )

        base_state_cell = base_state.serialize()
        base_boc = base_state_cell.to_boc()
        base_fhash = hashlib.sha256(base_boc).digest()
        base_rhash = base_state_cell.hash
        base_file: Path | None = state_dir / "basestate0.boc"
        _ = base_file.write_bytes(base_boc)
    else:
        # The basechain state was generated externally (its cells must be
        # pre-placed into each validator's celldb); no static BoC is written.
        assert external_basechain.gen_utime <= now_time, (
            f"external basechain gen_utime {external_basechain.gen_utime} is in the future"
            f" (now {now_time})"
        )
        base_rhash = external_basechain.root_hash
        base_fhash = external_basechain.file_hash
        base_file = None
        external_balance = external_basechain.total_balance

    # --- Now add param 12 (workchains) with base state hashes ---
    from block.generated import ConfigParam_12

    wc_descr = workchain_v2(
        enabled_since=now_time,
        monitor_min_split=config.monitor_min_split,
        min_split=config.split,
        max_split=config.split,
        basic=1,
        active=True,
        accept_msgs=True,
        flags=0,
        zerostate_root_hash=_bits256(base_rhash),
        zerostate_file_hash=_bits256(base_fhash),
        version=0,
        format=wfmt_basic(vm_version=-1, vm_mode=0),
        split_merge_timings=wc_split_merge_timings(
            split_merge_delay=20,
            split_merge_interval=20,
            min_split_merge_interval=10,
            max_split_merge_delay=1000,
        ),
        persistent_state_split_depth=0,
    )
    from block.generated import WorkchainDescr, WorkchainDescrType

    wc_dict: HashmapDict[WorkchainDescr] = HashmapDict(32, WorkchainDescrType())
    wc_dict[0] = wc_descr
    config_params.append(ConfigParam_12(workchains=wc_dict))

    # --- Add param 31 (special/fundamental addresses) ---
    # In Fift: wallet, smc3, and elector call make_special; config does NOT.
    from block.generated import ConfigParam_31

    special_dict: HashmapDict[None] = HashmapDict(256, UnitTypeInfo)
    special_dict[int.from_bytes(wallet_bp.address.hash_part, "big")] = None
    special_dict[int.from_bytes(elector_bp.address.hash_part, "big")] = None
    special_dict[int.from_bytes(smc3_addr.hash_part, "big")] = None
    config_params.append(ConfigParam_31(fundamental_smc_addr=special_dict))

    # --- Rebuild config blueprint with all params and deploy ---
    config_bp = ConfigBlueprint(config_params)
    zs.deploy(config_bp, ton(10))

    # --- Build masterchain shard state ---
    total_balance = sum(smc.balance for smc in zs.smcs)
    shard_accounts = _build_shard_accounts(zs.smcs, workchain_id=-1)

    # Get config dict from the config blueprint's state
    config_dict_cell = config_bp.state_init.data
    assert config_dict_cell is not None
    cs = config_dict_cell.begin_parse()
    config_dict_ref = cs.load_ref()
    config_addr_bits = _bits256_int(int.from_bytes(b"\x55" * 32, "big"))

    mc_extra = masterchain_state_extra(
        shard_hashes=ShardHashes({}),
        config=ConfigParams(
            config_addr=config_addr_bits,
            config=Ref(
                HashmapDict[Cell].type_info(32, CellRefType, allow_empty=False),
                HashmapDict[Cell].load_from(
                    config_dict_ref.begin_parse(), 32, CellRefType, allow_empty=False
                ),
            ),
        ),
        field=ref(
            Anon_10(
                flags=0,
                validator_info=validator_info(
                    validator_list_hash_short=compute_validator_set_hash(validator_keys),
                    catchain_seqno=0,
                    nx_cc_updated=True,
                ),
                prev_blocks=OldMcBlocksInfo(
                    field=HashmapDict(32, KeyExtBlkRef, KeyMaxLt, KeyMaxLtAug()),
                ),
                after_key_block=True,
                last_key_block=None,
                block_create_stats=None,
            )
        ),
        global_balance=_cc(total_balance + external_balance),
    )

    mc_state = shard_state(
        global_id=-777,
        shard_id=shard_ident(shard_pfx_bits=0, workchain_id=-1, shard_prefix=0),
        seq_no=0,
        vert_seq_no=0,
        gen_utime=now_time,
        gen_lt=0,
        min_ref_mc_seqno=0xFFFF_FFFF,
        out_msg_queue_info=ref(
            OutMsgQueueInfo(
                out_queue=OutMsgQueue({}),
                proc_info=ProcessedInfo({}),
                extra=None,
            )
        ),
        before_split=0,
        accounts=ref(shard_accounts),
        field=ref(
            Anon_4(
                overload_history=0,
                underload_history=0,
                total_balance=_cc(total_balance),
                total_validator_fees=_zero_cc(),
                libraries=_collect_public_libraries(zs.smcs),
                master_ref=None,
            )
        ),
        custom=ref(mc_extra),
    )

    mc_state_cell = mc_state.serialize()
    mc_boc = mc_state_cell.to_boc()
    mc_fhash = hashlib.sha256(mc_boc).digest()
    mc_rhash = mc_state_cell.hash
    _ = (state_dir / "zerostate.boc").write_bytes(mc_boc)

    return Zerostate(
        masterchain=WorkchainState(
            file=state_dir / "zerostate.boc",
            file_hash=mc_fhash,
            root_hash=mc_rhash,
        ),
        shardchain=WorkchainState(
            file=base_file,
            file_hash=base_fhash,
            root_hash=base_rhash,
        ),
        _wallet_bp=wallet_bp,
        _elector_bp=elector_bp,
        _config_bp=config_bp,
    )
