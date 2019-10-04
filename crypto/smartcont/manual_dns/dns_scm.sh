#!/bin/bash
#
# Wrapper on ./compile script for handling basic general operations
#/
OUTPUT_DIR=output
PK=scm.pk
INPUT_FC=dns.fc

COMMAND=$1
shift

print_command() {
  echo -e "\e[32m"${1}"\e[0m"
}

WORKDIR=$(realpath `dirname ${BASH_SOURCE[0]}`)
if [[ "$WORKDIR" != "`pwd`" ]]; then
  echo "ERROR: the script MUST be run inside its directory"
  echo "cd dir/"
  exit 1
fi

case $COMMAND in
  init)
    command="../compile -o $OUTPUT_DIR -k ${OUTPUT_DIR}/${PK} $INPUT_FC $@ -m messages/init.fif.sh"
    print_command "$command"
    echo
    ${command}
    ;;
  upgrade)
    command="../compile -o $OUTPUT_DIR/1 -k ${OUTPUT_DIR}/${PK} $INPUT_FC -a $OUTPUT_DIR/dns.addr -m messages/upgrade-scm.fif.sh $@"
    print_command "$command"
    echo
    ${command}
    ;;
  drop-data)
    command="../compile -o $OUTPUT_DIR/1 -k ${OUTPUT_DIR}/${PK} $INPUT_FC -a $OUTPUT_DIR/dns.addr -m messages/drop-data.fif.sh $@"
    print_command "$command"
    echo
    ${command}
    ;;
  register)
    command="../compile -o $OUTPUT_DIR/1 -k ${OUTPUT_DIR}/${PK} $INPUT_FC -a $OUTPUT_DIR/dns.addr -m messages/manual-register.fif.sh $@"
    print_command "$command"
    echo
    ${command}
    ;;
  transfer)
    command="../compile -o $OUTPUT_DIR/1 -k ${OUTPUT_DIR}/${PK} $INPUT_FC -a $OUTPUT_DIR/dns.addr -m messages/transfer.fif.sh $@"
    print_command "$command"
    echo
    ${command}
    ;;
  -h|--help|help|*)
    echo "This is a script for simplifying general operations of the DNS smart contract"
    echo "It creates all files into $OUTPUT_DIR dir"
    echo "If you would like to change predefined parameters - open the file and edit constants:"
    echo " * OUTPUT_DIR"
    echo " * PK"
    echo
    echo Usage: ./dns_scm.sh COMMAND
    echo
    echo COMMANDS:
    echo "  init                        create initialization message"
    echo "  upgrade [OPTIONS]...        create upgarde code message [DANGER]"
    echo "                                OPTIONS: ./dns_scm upgrade -h"
    echo "  drop-data [OPTIONS]...      create reinit c4 message (only for dev purposes) [DANGER]"
    echo "                                OPTIONS: ./dns_scm drop-data -h"
    echo "  register [OPTIONS]...       manual register resource record"
    echo "                                OPTIONS: ./dns_scm register -h"
    echo "  transfer [OPTIONS]...       transfer scm to the new ower [DANGER]"
    echo "                                OPTIONS: ./dns_scm transfer -h"
    echo
    echo "  help                        reads this message"

    ;;
esac
