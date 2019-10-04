#!/bin/bash
#
# Stack: ()
# Constants: code
# Variables: INPUT_PK, ($ADDR_FILE | ($ADDR, WC))
# Functions: error
#
usage() {
  echo
  echo "MESSAGE_OPTIONS:"
  echo "  --seqno=seqno                sequence number for preventing replay attacks"
  echo "                                 default=0"
}

opts=`getopt -o h \
      -l help,seqno: \
      -- $@`

eval set -- $opts
while [[ $# -gt 0 ]]; do
  case $1 in
    --seqno)
      SEQNO=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit
      ;;
    *)
      break
      ;;
  esac
done
shift

if [ -z "$ADDR_FILE" ]; then
  if [ -z "${ADDR}" ]; then
    error "Must be provided correct address, check -h"
    exit
  fi
  echo "0x${ADDR} constant w_addr dup ${WC} w_addr 2constant addr"
else
  cat <<EOF
    "TonUtil.fif" include
    "$ADDR_FILE" load-address-verbose 2constant addr
EOF
fi

SEQNO=${SEQNO:-0}
cat <<EOF
  "TonUtil.fif" include

  "${INPUT_PK}" load-keypair constant wallet_pk dup constant public_key
  <b ${SEQNO} 32 u, 1 4 u, code ref, b>
  dup ."Signing message: " <s csr. cr
  dup hash wallet_pk ed25519_sign_uint

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
    b{0} s,                // nothing$0
    b{0} s,                // body:(Either X ^X)
      swap B, swap <s s,
  b>
  cr
  dup <s ."UPGRADE Message:" cr csr.
EOF
