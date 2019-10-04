#!/bin/bash
#
# Runs all integ tests/* one by one and reports the status
# Provide summarized info about consumed gas
#
# stdout/stderr redirected to /dev/null
# For getting the information about the issue - runs the specific test manually with ./test
# Expects that ./test exists in the same directory as this file
# Needs to be run from the current folder
#
error() {
  echo -e "\e[31mERROR:\e[0m $1" 1>&2
}

WORKDIR=$(realpath `dirname ${BASH_SOURCE[0]}`)
if [[ "$WORKDIR" != "`pwd`" ]]; then
  error "ERROR: the script MUST be run inside its directory"
  echo "cd dir/"
  exit 1
fi

if [ -z `command -v func` ]; then
  error "func executable should be in PATH"
  HAS_ERROR=1
fi

if [ -z `command -v fift` ]; then
  error "fift executable should be in PATH"
  HAS_ERROR=1
fi

if [ ! -z $HAS_ERROR ]; then
  exit 1
fi

usage() {
  echo "  -p, --perf                   shows performance output"
  echo "  -h, --help                   display this help and exit"
}

DIR_TESTS=tests
if [ ! -d $DIR_TESTS ]; then
  echo "ERROR: script MUST be run from the file directory"
  exit 1
fi

opts=`getopt -o h,p \
      -l help,perf \
      -- $@`

eval set -- $opts

while [[ $# -gt 0 ]]; do
  case $1 in
    -h|--help)
      usage
      exit
      ;;
    -p|--perf)
      SHOW_PERF=1
      shift 2
      ;;
    *)
      break
      ;;
  esac
done
shift

DIR=`dirname ${BASH_SOURCE[0]}`
EXEC=../test
SPECS=( `ls $DIR_TESTS | grep -e \.fif$` )
INPUT_FC=${DIR}/dns.fc

if [ ! -f $EXEC ]; then
  echo "ERROR: Unable to locate ./test executable"
  exit 1
fi

for i in ${!SPECS[@]}; do
  f="${SPECS[i]}"
  spec="$DIR_TESTS/$f"
  if [ -z "$SHOW_PERF" ]; then
    ${EXEC} $INPUT_FC $spec 1>/dev/null 2>&1
    if [ $? -ne 0 ]; then
      echo -e " $i) \e[31mERROR:\e[0m ${EXEC} $INPUT_FC $spec"
    else
      echo -e " $i) \e[32mSUCCESS:\e[0m ${EXEC} $INPUT_FC $spec"
    fi
  else
    ${EXEC} $INPUT_FC $spec 2>&1 | grep "gas: used"
    if [ $PIPESTATUS -ne 0 ]; then
      echo -e " $i) \e[31mERROR:\e[0m ${EXEC} $INPUT_FC $spec"
    else
      echo -e " $i) \e[32mSUCCESS:\e[0m ${EXEC} $INPUT_FC $spec"
    fi
  fi
done
