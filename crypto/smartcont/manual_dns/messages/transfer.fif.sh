#!/bin/bash
#
# Message from transfering scm from one owner to another
#
# Stack: ()
# Constants:
# Variables: INPUT_PK, ($ADDR_FILE | ($ADDR, WC))
# Functions: error
#
usage() {
  echo
  echo "MESSAGE_OPTIONS:"
  echo "  --seqno=seqno                sequence number for preventing replay attacks"
  echo "                                 default=0"
  echo "  --to=uint32                  new owner public key for transer"
  echo "                                 exampe=0xB2CDAC979453F114372A88C38EF8E39C6235FAC17694262F0EA6240AB01B2C22"
  echo "                                 use 'show-public-key.fif' script for generating public key"
}

opts=`getopt -o h\
      -l help,seqno:,to: \
      -- $@`

eval set -- $opts
while [[ $# -gt 0 ]]; do
  case $1 in
    --seqno)
      SEQNO=$2;
      shift 2;
      ;;
    --to)
      TO=$2
      shift 2;
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

if [ -z "$TO" ]; then
  error "MUST provide new ower, check -h"
  exit 1
fi

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

  "${INPUT_PK}" load-keypair constant wallet_pk
  <b ${TO} 256 u, b>
  <b ${SEQNO} 32 u, 4 4 u, swap ref, b>
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
  dup <s ."Transfer Message:" cr csr.
EOF
