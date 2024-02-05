# Standalone LiteServer

## Setup

Run `./lite-server-daemon/lite-server -D <NODE-DB> --ip <IP>>:<PORT>` to generate config file.

Check:

- ADNL key
- LiteServer key / port
- IP

## Run

`./lite-server-daemon/lite-server -D <NODE-DB> -S <NODE-DB>/liteserver.json -C <GLOBAL-CONFIG-PATH> -v 3`

## Configurate sending external messages

1. Create keypair `generate-random-id -m keys -n master`
2. Save TCP port and hash of key (it'll be in output) to config of your node, don't forget to add it to `adnl`
   with `category` 1:
   ```
     "fullnodemasters" : [
      {
         "@type" : "engine.validator.fullNodeMaster",
         "port" : PORT GOES HERE,
         "adnl" : "KEYHASH GOES HERE"
      }
   ```

   ```
      "adnl" : [
      {
         "@type" : "engine.adnl",
         "id" : "KEYHASH GOES HERE",
         "category" : 0
      },
   ```
3. Obtain pub key from `master.pub`:
   ```
   import base64 as b
   b.b64encode(open("master.pub", "rb").read()[4:])
   ```
4. Put pub key to `liteserver.json` config with IP and port:
   ```
     "fullnodeslaves": [
    {
      "@type": "engine.validator.fullNodeSlave",
      "ip": INT IP GOES HERE,
      "port": PORT GOES HERE,
      "adnl": {
        "@type": "pub.ed25519",
        "key": "PUBKEY GOES HERE"
      }
    }
   ]
   ```

5. Enjoy of proxy external messages over slave node