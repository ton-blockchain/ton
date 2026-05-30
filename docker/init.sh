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

if [ -z "$QUIC_PORT" ]; then
    QUIC_PORT=31001
    echo -e "\e[1;33m[=]\e[0m Using default QUIC_PORT $QUIC_PORT udp"
else
    echo -e "\e[1;33m[=]\e[0m Using QUIC_PORT $QUIC_PORT udp"
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

if [[ "$PUBLIC_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    IFS='.' read -r IP1 IP2 IP3 IP4 <<< "$PUBLIC_IP"
    QUIC_IP_INT=$(( (IP1 << 24) + (IP2 << 16) + (IP3 << 8) + IP4 ))
    if [ "$QUIC_IP_INT" -gt 2147483647 ]; then
        QUIC_IP_INT=$((QUIC_IP_INT - 4294967296))
    fi

    QUIC_CONFIG_TMP=$(mktemp /var/ton-work/db/config.json.quic.XXXXXX)
    test $? -eq 0 || { echo "Cannot create temporary QUIC config file"; exit 2; }
    jq --argjson quic_ip "$QUIC_IP_INT" --argjson quic_port "$QUIC_PORT" '
      .addrs = (
        (.addrs // []) as $addrs
        | if ($addrs | any(.["@type"] == "engine.quicAddr" and .ip == $quic_ip))
          then ($addrs | map(
            if .["@type"] == "engine.quicAddr" and .ip == $quic_ip
            then . + {"port": $quic_port}
            else .
            end
          ))
          else $addrs + [{"@type":"engine.quicAddr","ip":$quic_ip,"port":$quic_port,"categories":[0, 1, 2, 3],"priority_categories":[]}]
          end
      )
    ' /var/ton-work/db/config.json > "$QUIC_CONFIG_TMP"
    test $? -eq 0 || { rm -f "$QUIC_CONFIG_TMP"; echo "Cannot apply QUIC address config"; exit 2; }
    mv "$QUIC_CONFIG_TMP" /var/ton-work/db/config.json
    test $? -eq 0 || { rm -f "$QUIC_CONFIG_TMP"; echo "Cannot replace config after QUIC update"; exit 2; }
    echo -e "\e[1;32m[+]\e[0m QUIC address configured: $PUBLIC_IP:$QUIC_PORT"
else
    echo -e "\e[1;31m[!]\e[0m PUBLIC_IP is not IPv4, skipping QUIC address configuration"
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
    SERVER_ID2=$(generate-random-id -m id -k ./server | sed -n '3p' | jq -r '.id')
    test $? -eq 0 || { echo "Cannot derive server public id from existing certificate"; exit 2; }
    if [ -z "$SERVER_ID2" ] || [ "$SERVER_ID2" = "null" ]; then
        echo "Cannot derive server public id from existing certificate"
        exit 2
    fi
else
    echo -e "\e[1;32m[+]\e[0m Generating and installing server certificate for remote control"
    read -r SERVER_ID1 SERVER_ID2 <<< $(generate-random-id -m keys -n server)
    echo "Server IDs: $SERVER_ID1 $SERVER_ID2"
    cp server /var/ton-work/db/keyring/$SERVER_ID1
    test $? -eq 0 || { echo "Cannot install server private key into keyring"; exit 2; }
fi

# Generating client certificate
if [ -f "./client" ]; then
    echo -e "\e[1;33m[=]\e[0m Found existing client certificate, skipping"
    CLIENT_ID2=$(generate-random-id -m id -k ./client | sed -n '3p' | jq -r '.id')
    test $? -eq 0 || { echo "Cannot derive client public id from existing certificate"; exit 2; }
    if [ -z "$CLIENT_ID2" ] || [ "$CLIENT_ID2" = "null" ]; then
        echo "Cannot derive client public id from existing certificate"
        exit 2
    fi
else
    read -r CLIENT_ID1 CLIENT_ID2 <<< $(generate-random-id -m keys -n client)
    echo -e "\e[1;32m[+]\e[0m Generated client private certificate $CLIENT_ID1 $CLIENT_ID2"
    echo -e "\e[1;32m[+]\e[0m Generated client public certificate"
fi

# Configure control interface and client permissions
CONTROL_CONFIG_TMP=$(mktemp /var/ton-work/db/config.json.control.XXXXXX)
test $? -eq 0 || { echo "Cannot create temporary control config file"; exit 2; }
jq --arg server_id "$SERVER_ID2" --arg client_id "$CLIENT_ID2" --argjson console_port "$CONSOLE_PORT" '
  .control = (
    (.control // []) as $control
    | ($control | map(select(.id != $server_id))) as $other_controls
    | ($control | map(select(.id == $server_id))[0] // {
        "@type":"engine.controlInterface",
        "id":$server_id,
        "port":$console_port,
        "allowed":[]
      }) as $server_control
    | ($server_control + {
        "@type":"engine.controlInterface",
        "id":$server_id,
        "port":$console_port,
        "allowed": (
          ($server_control.allowed // []) as $allowed
          | if ($allowed | any(.id == $client_id))
            then ($allowed | map(
              if .id == $client_id
              then . + {"@type":"engine.controlProcess","permissions":15}
              else .
              end
            ))
            else $allowed + [{"@type":"engine.controlProcess","id":$client_id,"permissions":15}]
            end
        )
      }) as $updated_server_control
    | $other_controls + [$updated_server_control]
  )
' /var/ton-work/db/config.json > "$CONTROL_CONFIG_TMP"
test $? -eq 0 || { rm -f "$CONTROL_CONFIG_TMP"; echo "Cannot apply control interface config"; exit 2; }
mv "$CONTROL_CONFIG_TMP" /var/ton-work/db/config.json
test $? -eq 0 || { rm -f "$CONTROL_CONFIG_TMP"; echo "Cannot replace config after control update"; exit 2; }

# Liteserver
if [ -z "$LITESERVER" ]; then
    echo -e "\e[1;33m[=]\e[0m Liteserver disabled"
else
    if [ -z "$LITE_PORT" ]; then
        LITE_PORT=30003
        echo -e "\e[1;33m[=]\e[0m Using default LITE_PORT $LITE_PORT tcp"
    else
        echo -e "\e[1;33m[=]\e[0m Using LITE_PORT $LITE_PORT tcp"
    fi

    if [ -f "./liteserver" ]; then
        echo -e "\e[1;33m[=]\e[0m Found existing liteserver certificate, skipping"
        LITESERVER_ID2=$(generate-random-id -m id -k ./liteserver | sed -n '3p' | jq -r '.id')
        test $? -eq 0 || { echo "Cannot derive liteserver public id from existing certificate"; exit 2; }
        if [ -z "$LITESERVER_ID2" ] || [ "$LITESERVER_ID2" = "null" ]; then
            echo "Cannot derive liteserver public id from existing certificate"
            exit 2
        fi
    else
        echo -e "\e[1;32m[+]\e[0m Generating and installing liteserver certificate for remote control"
        read -r LITESERVER_ID1 LITESERVER_ID2 <<< $(generate-random-id -m keys -n liteserver)
        echo "Liteserver IDs: $LITESERVER_ID1 $LITESERVER_ID2"
        cp liteserver /var/ton-work/db/keyring/$LITESERVER_ID1
        test $? -eq 0 || { echo "Cannot install liteserver private key into keyring"; exit 2; }
    fi

    LITESERVER_CONFIG_TMP=$(mktemp /var/ton-work/db/config.json.liteservers.XXXXXX)
    test $? -eq 0 || { echo "Cannot create temporary liteserver config file"; exit 2; }
    jq --arg liteserver_id "$LITESERVER_ID2" --argjson lite_port "$LITE_PORT" '
      .liteservers = (
        (.liteservers // []) as $liteservers
        | if ($liteservers | any(.id == $liteserver_id))
          then ($liteservers | map(
            if .id == $liteserver_id
            then . + {"port": $lite_port}
            else .
            end
          ))
          else $liteservers + [{"@type":"engine.liteServer","id":$liteserver_id,"port":$lite_port}]
          end
      )
    ' /var/ton-work/db/config.json > "$LITESERVER_CONFIG_TMP"
    test $? -eq 0 || { rm -f "$LITESERVER_CONFIG_TMP"; echo "Cannot apply liteserver config"; exit 2; }
    mv "$LITESERVER_CONFIG_TMP" /var/ton-work/db/config.json
    test $? -eq 0 || { rm -f "$LITESERVER_CONFIG_TMP"; echo "Cannot replace config after liteserver update"; exit 2; }
fi

echo -e "\e[1;32m[+]\e[0m Starting validator-engine:"
echo validator-engine -c /var/ton-work/db/config.json -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --state-ttl $STATE_TTL --archive-ttl $ARCHIVE_TTL --threads $THREADS --verbosity $VERBOSITY $CUSTOM_ARG
exec validator-engine -c /var/ton-work/db/config.json -C /var/ton-work/db/ton-global.config --db /var/ton-work/db --state-ttl $STATE_TTL --archive-ttl $ARCHIVE_TTL --threads $THREADS --verbosity $VERBOSITY $CUSTOM_ARG
