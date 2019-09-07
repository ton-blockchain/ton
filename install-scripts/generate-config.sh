#!/bin/bash

binary="../../ton-build/generate-random-id"

die() { 
  echo "$@" 1>&2 ; 
  exit 1; 
}

run_help() {
  echo "generates config for ton"
  echo "  server-list - list of core DHT servers"
  echo "  server-list.local - list of additional servers"
  echo "  also expects generate-random-id binary to be accessible by path '$binary'"
  die
}

get_line() {
  x=`grep -h -e "^$1 " server-list server-list.local`
  echo $x
}

get_ip_block() {
  echo $1 | cut -f $2 -d"."
}

get_ip() {
  ip=`echo $1 | cut -f 2 -d" "`
  a=$(get_ip_block $ip 1)
  b=$(get_ip_block $ip 2)
  c=$(get_ip_block $ip 3)
  d=$(get_ip_block $ip 4)
  ip=$(( $a * 256 * 256 * 256 + $b * 256 * 256 + $c * 256 + $d ))

  if [ "$ip" -gt "2147483647" ] ; then
    ip=$((256*256*256*256 - $ip))
    ip="-$ip"
  fi

  echo $ip
}

get_port() {
  echo $1 | cut -f 3 -d" "
}

get_pk() {
  #echo \'$1\' | cut -f 4 -d" "
  echo $1 | cut -f 4 -d" "
}

get_addr_list() {
  line=$1

  ip=$(get_ip "$line")
  port=$(get_port "$line")
  addr_list="{\"@type\":\"adnl.addressList\",\"version\":0,\"addrs\":[{\"ip\":$ip,\"port\":$port,\"@type\":\"adnl.address.udp\"}]}"
  
  echo $addr_list
}

get_node() {
  line=$1

  addr_list=$(get_addr_list "$line") 
  
  pk=$(get_pk "$line")

  res=`$binary -k $pk -a $addr_list | tail -1`
  echo $res
}

get_id_short() {
  pk=$(get_pk "$1")
  res=`$binary -k $pk | tail -1`
  echo $res
}


action=$1

if [ "$action" == "global" ] ; then

  [ -f "$binary" ] || die "can not file '$binary'"
  [ -f "server-list" ] || die "can not file 'server-list'"
  [ -f "server-list.local" ] || die "can not file 'server-list.local'"

  if [ "x$2" == "x" ] ; then
    config_filename="ton-global.config.json"
  else 
    config_filename=$2
  fi
  list=`cat server-list | awk '{ print $1}'`

  config='{"@type":"config.global","dht":{"@type":"dht.config.global","k":10,"a":3,"static_nodes":{"@type":"adnl.nodes","nodes":'

  cnt=0

  for name in $list ; do
    if [ $cnt -eq 0 ] ; then
      config="$config["
    else
      config="$config,"
    fi
    cnt=$(($cnt + 1))
    line=$(get_line $name)
    node=$(get_node "$line")
    config="$config$node"
  done

  config="$config]}}}"
  #name=$1
  #line=$(get_line $name)
  #echo $line
  #res=$(get_node "$line")
  #echo $res
  echo $config > $config_filename
elif [ "$action" == "local" ] ; then

  [ -f "$binary" ] || die "can not file '$binary'"
  [ -f "server-list" ] || die "can not file 'server-list'"
  [ -f "server-list.local" ] || die "can not file 'server-list.local'"

  name=$2
  if [ "x$3" == "x" ] ; then
    config_filename="ton-local.config.$name.json"
  else 
    config_filename=$3
  fi
  line=$(get_line $name)
  config='{"@type":"config.local","local_ids":['
  id=$(get_pk "$line")
  id_short=$(get_id_short "$line")
  addr_list=$(get_addr_list "$line")
  config="$config{\"@type\":\"id.config.local\",\"id\":$id,\"addr_list\":$addr_list}]"
  config="$config,\"net\":$addr_list"
  config="$config,\"dht\":[{\"@type\":\"dht.config.local\",\"id\":$id_short}]"
  config="$config,\"dht_random\":{\"@type\":\"config.dht.random\",\"cnt\":0,\"addr_list\":$addr_list}"
  config="$config,\"public_overlays\":[{\"@type\":\"overlay.config.local\",\"name\":\"testoverlay\",\"id\":$id_short}]"
  config="$config,\"public_overlays_random\":{\"@type\":\"overlay.config.random\",\"cnt\":0,\"name\":\"testoverlay\",\"addr_list\":$addr_list}"
  config="$config}"

  echo $config > $config_filename
elif [ "$action" == "-h" ] ; then
  run_help
elif [ "$action" == "--help" ] ; then
  run_help
fi
