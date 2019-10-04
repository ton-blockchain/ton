#!/bin/bash
#
# Current message registers domain (or list of domains into smartcontract
#
# Stack: ()
# Constants: addr wallet_pk
# Variables: INPUT_PK, ($ADDR_FILE | ($ADDR, WC))
# Functions: error
#
usage() {
  echo
  echo "MESSAGE_OPTIONS:"
  echo "  --seqno=seqno                sequence number for preventing replay attacks"
  echo "                                 default=0"
  echo "  -d,--domain=domain           domain"
  echo "  -r,--resource=wc:addr        resource related to registered domain, in general scm address in wc:addr format"
  echo "  -c,--category=number         resource category, -1 is for NS"
  echo "                                 default=1"
}

opts=`getopt -o h,d:,r:,c:\
      -l help,domain:,resource:,category:,seqno: \
      -- $@`

eval set -- $opts
while [[ $# -gt 0 ]]; do
  case $1 in
    --seqno)
      SEQNO=$2
      shift 2
      ;;
    -d|--domain)
      DOMAIN=$2
      shift 2
      ;;
    -r|--resource)
      RESOURCE=(`IFS=":"; echo $2`)

      if [ ${#RESOURCE[@]} -ne 2 ]; then
        error "Wrong format of the resource $2. MUST be wc:addr"
        exit 1
      fi
      shift 2
      ;;
    -c|--category)
      CATEGORY=$2
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

if [ -z "$DOMAIN" ]; then
  error "MUST provide a domain, check -h"
  usage
  exit 1
fi

if [ -z "$RESOURCE" ]; then
  error "MUST provide a resource, check -h"
  usage
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

CATEGORY=${CATEGORY:-1}
SEQNO=${SEQNO:-0}
cat <<EOF
  "TonUtil.fif" include

  "${INPUT_PK}" load-keypair constant wallet_pk dup constant public_key
  <b b{001} s, ${RESOURCE[0]} 0x${RESOURCE[1]} addr, b>
  <b b{010} s, swap ref, b>
  <b "${DOMAIN}" $, b>
  <b ${SEQNO} 32 u, 2 4 u, ${CATEGORY} 16 i, swap ref, swap ref, b>
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
  dup <s ."Message ResourceRegister:" cr csr.
EOF
