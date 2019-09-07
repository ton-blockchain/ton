#!/bin/bash


host=$1
user="root"
ssh=$user@$host

die() { 
  echo "$@" 1>&2 ; 
  exit 1; 
}

run_help() {
  echo "tries to initialize new server"
  echo "uses these files: "
  echo "  tonkey - RSA key that allows to access git repository"
  echo "  server-list - list of core DHT servers"
  echo "  server-list.local - list of additional servers"
  die
}

if [ "$1" == "-h" ] ; then 
  run_help
fi

if [ "$1" == "--help" ] ; then 
  run_help
fi

scp tonkey $ssh:.ssh/id_rsa || die "cannot copy id_rsa"
ssh $ssh chmod 0600 .ssh/id_rsa || die "cannot chmod id_rsa"
scp known_hosts $ssh:.ssh/known_hosts || die "cannot copy known_hosts"
ssh $ssh "apt-get update && apt-get -y install git libssl-dev cmake g++ gperf libz-dev" || die "cannot install packets"
ssh $ssh "git clone git@bitbucket.org:toin/ton.git ; cd ton && git submodule init ; git submodule update && cd third-party/libraptorq && git submodule init && git submodule update" || die "cannot clone git"
ssh $ssh "cd ton && git submodule update" || die "can not init submodules"
ssh $ssh "if [ ! -d ton-build ]; then mkdir ton-build ; fi"
ssh $ssh "cd ton-build && cmake ../ton" || die "cannot prepare for build"
ssh $ssh "cd ton-build && make -j 8 test-node" || die "cannot build"
sh generate-config.sh global ton-global.config.json || die "cannot create global config"
sh generate-config.sh local $host ton-local.config.json || die "cannot create local config"
scp ton-global.config.json ton-local.config.json $ssh:
