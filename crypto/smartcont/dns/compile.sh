#!/bin/bash
#
# Requires fift and fc compilers in PATH
#
# HOWTO:
# ./compile.sh input.fc output.pk
#
INPUT_FC=${1:-dns.fc}
OUTPUT_PK=${2:-output.pk}
WC=-1

DIR=`dirname ${BASH_SOURCE[0]}`
OUTPUT_DIR=${DIR}/output
mkdir -p $OUTPUT_DIR

OUTPUT_FIFT=${OUTPUT_DIR}/dns.fif
OUTPUT_BOC=${OUTPUT_DIR}/dns.boc

func -AP ${DIR}/../stdlib.fc ${INPUT_FC} | tee $OUTPUT_FIFT

cat >> $OUTPUT_FIFT <<EOF
  dup <s constant code
EOF

cat >> $OUTPUT_FIFT <<EOF
  "TonUtil.fif" include
  <b
    0 32 u,
    "${OUTPUT_DIR}/${OUTPUT_PK}" load-generate-keypair
     constant wallet_pk
     B,
  b> swap
EOF


cat >> $OUTPUT_FIFT <<EOF
  "TonUtil.fif" include
  ${WC} constant wc

  // 0x97a4e0c7e4dd767fcac0c39dbbffdf8af993f898ae9ebb73d1e902ae8ea75f9e constant w_addr
  // 0xD7C3430AB9CA1AAD4B6783AEC345FAD10367BB9425081A8B3547A4F08C56E956 constant w_addr
  0x6e204472be281bc43a5ba5d4c54cbea0fe0b3efe2b30ffb364573ae22484c5af constant w_addr

  cr ."-- FIFT extension --" cr cr
  "TonUtil.fif" include
  // StateInit
  <b
    // split_depth:(Maybe (## 5)) special:(Maybe TickTock)
    b{00} s,                // nothing$0
    // code:(Maybe ^Cell)
    b{1} s,                 // just$1
      swap ref,             // ^Cell
    b{1} s,                 // data:(Maybe ^Cell)
       swap ref,
    b{0} s,                 // library:(HashmapE 256 SimpleLib)
  b>
  ."StateInit:" cr dup <s csr.
  ."Hash(StateInit): " dup hash . cr
  dup ."Smart contract addr: " hash wc swap 2dup 2constant addr .addr cr
  // dup ."Smart contract addr: " wc w_addr 2dup 2constant addr .addr cr
  ."Smart contract non-bounceable: " addr 7 .Addr cr
  ."Smart contract bounceable: " addr 6 .Addr cr

  // pk load-keypair nip constant wallet_pk
  <b 0 32 u, b>
  dup ."signing message: " <s csr. cr
  dup hash wallet_pk ed25519_sign_uint rot
EOF

cat >> $OUTPUT_FIFT <<EOF
  <b
    // info:CommonMsgInfo
    b{10} s,               // ext_in_msg_info$10
      // src:MsgAddressExt
      b{00} s,             // addr_none$00
      // dest:MsgAddressInt
      b{10} s,             // addr_std$10
        b{0} s,            // anycast:(Maybe Anycast)
        addr addr,         // workchain_id:int8 address:bits256
      // import_fee:Grams
      0 Gram,              // (VarUInteger 16) // b{0000} s,
    // init:(Maybe (Either StateInit ^StateInit))
    b{1} s,                // just$1
      b{1} s,              // left$0
        swap ref,         // ^StateInit
    // body:(Either X ^X)
    b{0} s,               // left$0 0
      swap B, swap <s s,
  b>
  dup <s ."Message X:" cr csr.
EOF

cat >> $OUTPUT_FIFT <<EOF
dup ."External message for initialization is " <s csr. cr

2 boc+>B "${OUTPUT_BOC}" tuck B>file
."Boc is generated: ${OUTPUT_BOC}" cr
EOF

fift -v3 $OUTPUT_FIFT
