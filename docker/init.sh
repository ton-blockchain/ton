#!/usr/bin/env bash

if [ ! -z "$TEST" ]; then
  echo -e "Running simple validator-engine test..."
  validator-engine -h
  test $? -eq 2 || { echo "simple validator-engine test failed"; exit 1; }
  exit 0;
fi

# global config
if [ ! -z "$GLOBAL_CONFIG_URL" ]; then
    echo -e "\e[1;32m[+]\e[0m Downloading provided global config."
    wget -q $GLOBAL_CONFIG_URL -O /var/ton-work/db/ton-global.config
else
    echo -e "\e[1;33m[=]\e[0m No global config provided, downloading mainnet default."
    wget -q https://api.tontech.io/ton/wallet-mainnet.autoconf.json -O /var/ton-work/db/ton-global.config
fi

if [ -z "$VALIDATOR_PORT" ]; then
    VALIDATOR_PORT=30001
    echo -e "\e[1;33m[=]\e[0m Using default VALIDATOR_PORT $VALIDATOR_PORT udp"
else
    echo -e "\e[1;33m[=]\e[0m Using VALIDATOR_PORT $VALIDATOR_PORT udp"
fi

# Init local config with IP:PORT
if [ ! -z "$PUBLIC_IP" ]; then
    echo -e "\e[1;32m[+]\e[0m Using provided IP: $PUBLIC_IP:$VALIDATOR_PORT"
else
    echo -e "\e[1;31m[!]\e[0m No PUBLIC_IP provided, exiting..."
    exit 1
fi

if [ ! -f "/var/ton-work/db/config.json" ]; then
  echo -e "\e[1;32m[+]\e[0m Initializing validator-engine:"
  echo validator-engine -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --ip "$PUBLIC_IP:$VALIDATOR_PORT"
  validator-engine -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --ip "$PUBLIC_IP:$VALIDATOR_PORT"
  test $? -eq 0 || { echo "Cannot initialize validator-engine"; exit 2; }
fi

if [ ! -z "$DUMP_URL" ]; then
    echo -e "\e[1;32m[+]\e[0m Using provided dump $DUMP_URL"
    if [ ! -f "dump_downloaded" ]; then
      echo -e "\e[1;32m[+]\e[0m Downloading dump..."
      curl --retry 10 --retry-delay 30 -Ls $DUMP_URL | pv | plzip -d -n8 | tar -xC /var/ton-work/db
      touch dump_downloaded
    else
      echo -e "\e[1;32m[+]\e[0m Dump has been already used."
    fi
fi

if [ -z "$STATE_TTL" ]; then
    STATE_TTL=86400
    echo -e "\e[1;33m[=]\e[0m Using default STATE_TTL $STATE_TTL"
else
    echo -e "\e[1;33m[=]\e[0m Using STATE_TTL $STATE_TTL"
fi

if [ -z "$ARCHIVE_TTL" ]; then
    ARCHIVE_TTL=86400
    echo -e "\e[1;33m[=]\e[0m Using default ARCHIVE_TTL $ARCHIVE_TTL"
else
    echo -e "\e[1;33m[=]\e[0m Using ARCHIVE_TTL $ARCHIVE_TTL"
fi

if [ -z "$THREADS" ]; then
    THREADS=8
    echo -e "\e[1;33m[=]\e[0m Using default THREADS $THREADS"
else
    echo -e "\e[1;33m[=]\e[0m Using THREADS $THREADS"
fi

if [ -z "$VERBOSITY" ]; then
    VERBOSITY=3
    echo -e "\e[1;33m[=]\e[0m Using default VERBOSITY $VERBOSITY"
else
    echo -e "\e[1;33m[=]\e[0m Using VERBOSITY $VERBOSITY"
fi

if [ -z "$CONSOLE_PORT" ]; then
    CONSOLE_PORT=30002
    echo -e "\e[1;33m[=]\e[0m Using default CONSOLE_PORT $CONSOLE_PORT tcp"
else
    echo -e "\e[1;33m[=]\e[0m Using CONSOLE_PORT $CONSOLE_PORT tcp"
fi

# Generating server certificate
if [ -f "./server" ]; then
    echo -e "\e[1;33m[=]\e[0m Found existing server certificate, skipping"
else
    echo -e "\e[1;32m[+]\e[0m Generating and installing server certificate for remote control"
    read -r SERVER_ID1 SERVER_ID2 <<< $(generate-random-id -m keys -n server)
    echo "Server IDs: $SERVER_ID1 $SERVER_ID2"
    cp server /var/ton-work/db/keyring/$SERVER_ID1
fi

# Generating client certificate
if [ -f "./client" ]; then
    echo -e "\e[1;33m[=]\e[0m Found existing client certificate, skipping"
else
    read -r CLIENT_ID1 CLIENT_ID2 <<< $(generate-random-id -m keys -n client)
    echo -e "\e[1;32m[+]\e[0m Generated client private certificate $CLIENT_ID1 $CLIENT_ID2"
    echo -e "\e[1;32m[+]\e[0m Generated client public certificate"
    # Adding client permissions
    sed -e "s/CONSOLE-PORT/\"$(printf "%q" $CONSOLE_PORT)\"/g" -e "s~SERVER-ID~\"$(printf "%q" $SERVER_ID2)\"~g" -e "s~CLIENT-ID~\"$(printf "%q" $CLIENT_ID2)\"~g" /var/ton-work/scripts/control.template > control.new
    sed -e "s~\"control\"\ \:\ \[~$(printf "%q" $(cat control.new))~g" /var/ton-work/db/config.json > config.json.new
    mv config.json.new /var/ton-work/db/config.json
fi

# Liteserver
if [ -z "$LITESERVER" ]; then
    echo -e "\e[1;33m[=]\e[0m Liteserver disabled"
else
    if [ -f "./liteserver" ]; then
        echo -e "\e[1;33m[=]\e[0m Found existing liteserver certificate, skipping"
    else
        echo -e "\e[1;32m[+]\e[0m Generating and installing liteserver certificate for remote control"
        read -r LITESERVER_ID1 LITESERVER_ID2 <<< $(generate-random-id -m keys -n liteserver)
        echo "Liteserver IDs: $LITESERVER_ID1 $LITESERVER_ID2"
        cp liteserver /var/ton-work/db/keyring/$LITESERVER_ID1

        if [ -z "$LITE_PORT" ]; then
            LITE_PORT=30003
            echo -e "\e[1;33m[=]\e[0m Using default LITE_PORT $LITE_PORT tcp"
        else
            echo -e "\e[1;33m[=]\e[0m Using LITE_PORT $LITE_PORT tcp"
        fi

        LITESERVERS=$(printf "%q" "\"liteservers\":[{\"id\":\"$LITESERVER_ID2\",\"port\":\"$LITE_PORT\"}")
        sed -e "s~\"liteservers\"\ \:\ \[~$LITESERVERS~g" /var/ton-work/db/config.json > config.json.liteservers
        mv config.json.liteservers /var/ton-work/db/config.json
    fi
fi

echo -e "\e[1;32m[+]\e[0m Starting validator-engine:"
echo validator-engine -c /var/ton-work/db/config.json -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --state-ttl $STATE_TTL --archive-ttl $ARCHIVE_TTL --threads $THREADS --verbosity $VERBOSITY $CUSTOM_ARG
exec validator-engine -c /var/ton-work/db/config.json -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --state-ttl $STATE_TTL --archive-ttl $ARCHIVE_TTL --threads $THREADS --verbosity $VERBOSITY $CUSTOM_ARG
