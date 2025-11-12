from dataclasses import dataclass
from pathlib import Path

from .install import Install, run_fift
from .key import Key
from .tl import ton_api


def _shard_json_repr(shard: int):
    if shard >= (1 << 63):
        return shard - (1 << 64)
    return shard


@dataclass
class NetworkConfig:
    monitor_min_split: int = 0
    split: int = 0
    global_version: int = 11
    shard_validators: int = 1
    block_limit_mul: int = 1
    mc_catchain_lifetime: int = 250
    shard_catchain_lifetime: int = 250


@dataclass
class WorkchainState:
    file: Path
    file_hash: bytes
    root_hash: bytes


@dataclass
class Zerostate:
    masterchain: WorkchainState
    shardchain: WorkchainState

    def as_block(self):
        return ton_api.TonNode_blockIdExt(
            workchain=-1,
            shard=_shard_json_repr(0x8000_0000_0000_0000),
            seqno=0,
            root_hash=self.masterchain.root_hash,
            file_hash=self.masterchain.file_hash,
        )

    def as_validator_config(self):
        return ton_api.Validator_config_global(zero_state=self.as_block())


_TEMPLATE = """
"TonUtil.fif" include
"Asm.fif" include
"Lists.fif" include
"FiftExt.fif" include

256 1<<1- 15 / constant AllOnes

wc_master setworkchain
-777 setglobalid   // negative value means a test instance of the blockchain

// Initial state of Workchain 0 (Basic workchain)

0 mkemptyShardState

{{ <b x{{a7}} s, 5 roll 32 u, 4 roll 8 u, 3 roll 8 u, rot 8 u, x{{e000}} s,
  3 roll 256 u, rot 256 u, 0 32 u, x{{1}} s, -1 32 i, 0 64 u, x{{0}} s, 20 32 u, 20 32 u, 10 32 u, 1000 32 u, 0 8 u, b>
  dup isWorkchainDescr? not abort"invalid WorkchainDescr created"
  <s swap workchain-dict @ 32 idict!+ 0= abort"cannot add workchain"
  workchain-dict !
}} : add-std-workchain-v2

dup dup 31 boc+>B dup "basestate0.boc" B>file
Bhashu dup =: basestate0_fhash 256 u>B "basestate0.fhash" B>file
hashu dup =: basestate0_rhash 256 u>B "basestate0.rhash" B>file
basestate0_rhash basestate0_fhash now {monitor_min_split} {split} dup 0 add-std-workchain-v2
config.workchains!

// SmartContract #1 (Simple wallet)

<{{ SETCP0 DUP IFNOTRET // return if recv_internal
   DUP 85143 INT EQUAL IFJMP:<{{ // "seqno" get-method
     DROP c4 PUSHCTR CTOS 32 PLDU  // cnt
   }}>
   INC 32 THROWIF  // fail unless recv_external
   512 INT LDSLICEX DUP 32 PLDU   // sign cs cnt
   c4 PUSHCTR CTOS 32 LDU 256 LDU ENDS  // sign cs cnt cnt' pubk
   s1 s2 XCPU            // sign cs cnt pubk cnt' cnt
   EQUAL 33 THROWIFNOT   // ( seqno mismatch? )
   s2 PUSH HASHSU        // sign cs cnt pubk hash
   s0 s4 s4 XC2PU        // pubk cs cnt hash sign pubk
   CHKSIGNU              // pubk cs cnt ?
   34 THROWIFNOT         // signature mismatch
   ACCEPT
   SWAP 32 LDU NIP 8 LDU LDREF ENDS      // pubk cnt mode msg
   SWAP SENDRAWMSG       // pubk cnt ; ( message sent )
   INC NEWC 32 STU 256 STU ENDC c4 POPCTR
}}>c
// code
<b 0 32 u,
   "main-wallet.pk" load-generate-keypair drop
   B,
b> // data
Libs{{
  x{{ABACABADABACABA}} s>c public_lib
  x{{1234}} x{{5678}} |_ s>c private_lib
}}Libs  // libraries
GR$4999990000 // balance
0 // split_depth
0 // ticktock
AllOnes 0 * // address
6 // mode: create+setaddr
register_smc
dup make_special dup constant smc1_addr
Masterchain over
2dup ."wallet address = " .addr cr 2dup 6 .Addr cr
"main-wallet.addr" save-address-verbose

// SmartContract #3
PROGRAM{{
  recv_internal x{{}} PROC
  run_ticktock PROC:<{{
    c4 PUSHCTR CTOS 32 LDU 256 LDU ENDS
    NEWC ROT INC 32 STUR OVER 256 STUR ENDC
    c4 POPCTR
    // first 32 bits of persistent data have been increased
    // remaining 256 bits with an address have been fetched
    // create new empty message with 0.1 Grams to that address
    NEWC b{{00100010011111111}} STSLICECONST TUCK 256 STU
    100000000 INT STGRAMS  // store 0.1 Grams
    1 4 + 4 + 64 + 32 + 1+ 1+ INT STZEROES ENDC
    // send raw message from Cell
    ZERO SENDRAWMSG
    -17 INT 256 STIR 130000000 INT STGRAMS
    107 INT STZEROES ENDC
    ZERO // another message with 0.13 Grams to account -17
    NEWC b{{11000100100000}} "test" $>s |+ STSLICECONST
    123456789 INT STGRAMS
    107 INT STZEROES "Hello, world!" $>s STSLICECONST ENDC
    ZERO SENDRAWMSG SENDRAWMSG // external message to address "test"
  }}>
}}END>c
// code
<b x{{11EF55AA}} s, smc1_addr 256 u, b> // data
// empty_cell // libraries
Libs{{
  x{{ABACABADABACABA}} s>c public_lib
  x{{1234}} x{{5678}} |_ s>c public_lib
}}Libs  // libraries
GR$1 // balance
0 // split_depth
3 // ticktock: tick
2 // mode: create
register_smc
dup make_special dup constant smc3_addr
."address = " 64x. cr


/*
 *
 * SmartContract #4 (elector)
 *
 */
"auto/elector-code.fif" include   // code in separate source file
<b 0 1 1+ 1+ 4 + 32 + u, 0 256 u, b>  // data: dict dict dict grams uint32 uint256
empty_cell  // libraries
GR$10  // balance: 10 grams
0 // split_depth
2 // ticktock: tick
AllOnes 3 * // address: -1:333...333
6 // mode: create + setaddr
register_smc
dup make_special dup constant smc4_addr dup constant elector_addr
Masterchain swap
."elector smart contract address = " 2dup .addr cr 2dup 7 .Addr cr
"elector" +".addr" save-address-verbose

/*
 *
 * Configuration Parameters
 *
 */
// version capabilities
{global_version} capCreateStats capBounceMsgBody or capReportVersion or capShortDequeue or 64 or 128 or config.version!
// max-validators max-main-validators min-validators
1000 1000 1000 config.validator_num!
// min-stake max-stake min-total-stake max-factor
GR$10000 GR$100000 GR$10000 sg~10 config.validator_stake_limits!
// elected-for elect-start-before elect-end-before stakes-frozen-for
// 400000 200000 4000 400000 config.election_params!
// 200 190 10 10 config.election_params!  // DEBUG
2400 800 60 300 config.election_params!
// config-addr = -1:5555...5555
AllOnes 5 * constant config_addr
config_addr config.config_smc!
// elector-addr
elector_addr config.elector_smc!

// 1 sg* 100 sg* 1000 sg* 1000000 sg* config.storage_prices!  // old values (too high)
1 500 1000 500000 config.storage_prices!
config.special!

// gas_price gas_limit special_gas_limit gas_credit block_gas_limit freeze_due_limit delete_due_limit flat_gas_limit flat_gas_price --
10 sg* 1 *M dup   10000 1000 *M GR$0.1 GR$1.0 100 1000 config.gas_prices!
10 sg* 1 *M 20 *M 10000 1000 *M GR$0.1 GR$1.0 100 1000 config.mc_gas_prices!
// lump_price bit_price cell_price ihr_factor first_frac next_frac
100 10 sg* 10 sg* 3/2 sg*/ 1/3 sg*/ 1/3 sg*/ config.fwd_prices!
100 10 sg* 10 sg* 3/2 sg*/ 1/3 sg*/ 1/3 sg*/ config.mc_fwd_prices!
// mc-cc-lifetime sh-cc-lifetime sh-val-lifetime sh-val-num mc-shuffle
{mc_catchain_lifetime} {shard_catchain_lifetime} 1000 {shard_val} true config.catchain_params!

// round-candidates next-cand-delay-ms consensus-timeout-ms fast-attempts attempt-duration cc-max-deps max-block-size max-collated-size new-cc-ids
// proto-version catchain-max-blocks-coeff
<b x{{d9}} s, 1 8 u, 3 8 u, 2000 32 u, 16000 32 u, 3 32 u, 8 32 u, 4 32 u, 4 *Mi 32 u, 4 *Mi 32 u, 5 16 u, 0 32 u, b>
29 config!
// 3 2000 16000 3 8 4 960 *Mi 960 *Mi true config.consensus_params!

{{ <b x{{5e}} s, 3 roll param_limits, rot param_limits, swap param_limits,
  x{{d3}} s, 200000 32 u, 30 32 u, b>
}} : make-block-limits-v2

128 *Ki 512 *Ki {block_limit_mul} * 1 *Mi {block_limit_mul} * triple  // [ underload soft hard ] : block bytes limit
2000000 100000000 100000000 triple  // gas limits
1000 500000 1000000 triple        // lt limits
triple dup
// untriple make-block-limits-v2 22 config!
// untriple make-block-limits-v2 23 config!
untriple make-block-limits 22 config!
untriple make-block-limits 23 config!

GR$1.7 GR$1 config.block_create_fees!
// smc1_addr config.collector_smc!
smc1_addr config.minter_smc!

1000000000000 -17 of-cc 666666666666 239 of-cc cc+ config.to_mint!

( 0 1 9 10 12 14 15 16 17 18 20 21 22 23 24 25 28 34 ) config.mandatory_params!
( -999 -1000 -1001 0 1 9 10 12 14 15 16 17 32 34 36 ) config.critical_params!

// [ min_tot_rounds max_tot_rounds min_wins max_losses min_store_sec max_store_sec bit_pps cell_pps ]
// first for ordinary proposals, then for critical proposals
_( 2 3 2 2 1000000 10000000 1 500 )
_( 4 7 4 2 5000000 20000000 2 1000 )
config.param_proposals_setup!

// deposit bit_pps cell_pps
GR$100 1 500 config.complaint_prices!

{validators}
now dup 3600 + {mc_validators} config.validators!

{{
  =: data
  <b @' data B, b> <s
    // b{{00000}} // old
    b{{00000000}} // new
}} : collator-entry
{{ -rot dup sbits rot swap [[ <{{ DICTSET }}>s ]] 0 runvmx abort"dict-insert failed" }} : dict-insert

/*
 *
 * SmartContract #5 (Configuration smart contract)
 *
 */
"auto/config-code.fif" include   // code in separate source file
<b configdict ref,  // initial configuration
   0 32 u,          // seqno
   "config-master" +".pk" load-generate-keypair drop
   B,
   dictnew dict,   // vote dict
b> // data
empty_cell  // libraries
GR$10  // balance
0 1 config_addr 6 register_smc  // tock
dup set_config_smc
Masterchain swap
."config smart contract address = " 2dup .addr cr 2dup 7 .Addr cr
"config-master" +".addr" save-address-verbose
// Other data

/*
 *
 *  Create state
 *
 */

create_state
dup 31 boc+>B dup "zerostate.boc" B>file
Bhashu dup =: zerostate_fhash 256 u>B "zerostate.fhash" B>file
hashu dup =: zerostate_rhash 256 u>B "zerostate.rhash" B>file
"""


def create_zerostate(
    install: Install, state_dir: Path, config: NetworkConfig, validator_keys: list[Key]
) -> Zerostate:
    keys: list[str] = []
    for key in validator_keys:
        keys.append(f"B{{{key.public_key.key.hex()}}} 17 add-validator")

    run_fift(
        install,
        _TEMPLATE.format(
            monitor_min_split=config.monitor_min_split,
            split=config.split,
            global_version=config.global_version,
            shard_val=config.shard_validators,
            block_limit_mul=config.block_limit_mul,
            validators="\n".join(keys),
            mc_validators=len(keys),
            mc_catchain_lifetime=config.mc_catchain_lifetime,
            shard_catchain_lifetime=config.shard_catchain_lifetime,
        ),
        state_dir,
    )

    return Zerostate(
        masterchain=WorkchainState(
            file=state_dir / "zerostate.boc",
            file_hash=(state_dir / "zerostate.fhash").read_bytes(),
            root_hash=(state_dir / "zerostate.rhash").read_bytes(),
        ),
        shardchain=WorkchainState(
            file=state_dir / "basestate0.boc",
            file_hash=(state_dir / "basestate0.fhash").read_bytes(),
            root_hash=(state_dir / "basestate0.rhash").read_bytes(),
        ),
    )
