#!/usr/bin/env bash

# global config 
if [ ! -z "$GCONFURL" ]; then
    echo -e "\e[1;32m[+]\e[0m Downloading provided global config."
    wget -q $GCONFURL -O /var/ton-work/db/ton-global.config
else
    echo -e "\e[1;33m[=]\e[0m No global config provided, downloading default."
    wget -q https://api.tontech.io/ton/wallet-mainnet.autoconf.json -O /var/ton-work/db/ton-global.config
fi

# Init local config with IP:PORT
if [ ! -z "$PUBLIC_IP" ]; then
    if [ -z "$CONSOLE_PORT" ]; then
        CONSOLE_PORT="43678"
    fi
    echo -e "\e[1;32m[+]\e[0m Using provided IP: $PUBLIC_IP:$CONSOLE_PORT"
    validator-engine -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --ip "$PUBLIC_IP:$CONSOLE_PORT"
else
    echo -e "\e[1;31m[!]\e[0m No IP:PORT provided, exiting"
    exit 1
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
    sed -e "s/CONSOLE-PORT/\"$(printf "%q" $CONSOLE_PORT)\"/g" -e "s~SERVER-ID~\"$(printf "%q" $SERVER_ID2)\"~g" -e "s~CLIENT-ID~\"$(printf "%q" $CLIENT_ID2)\"~g" control.template > control.new
    sed -e "s~\"control\"\ \:\ \[~$(printf "%q" $(cat control.new))~g" config.json > config.json.new
    mv config.json.new config.json
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
            LITE_PORT="43679"
        fi
        LITESERVERS=$(printf "%q" "\"liteservers\":[{\"id\":\"$LITESERVER_ID2\",\"port\":\"$LITE_PORT\"}")
        sed -e "s~\"liteservers\"\ \:\ \[~$LITESERVERS~g" config.json > config.json.liteservers
        mv config.json.liteservers config.json
    fi
fi

echo -e "\e[1;32m[+]\e[0m Running validator-engine"

exec validator-engine -c /var/ton-work/db/config.json -C /var/ton-work/db/ton-global.config --db /var/ton-work/db