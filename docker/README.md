# Official TON Docker image
## Prerequisites

The TON node, whether it is validator or fullnode, requires a public IP address. 
If your server is within an internal network or kubernetes you have to make sure that the required ports are available from the outside.

## Docker

### Installation
```docker pull ghcr.io/ton-blockchain/ton:latest```

### Configuration
TON validator-engine supports number of command line parameters, 
these parameters can be handed over to the container via environment variables. 
Below is the list of supported arguments and their default values:

| Argument       | Description                                                                                                                                                                         | Mandatory? |                      Default value                      |
|:---------------|:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:----------:|:-------------------------------------------------------:|
| PUBLIC_IP      | This will be a public IP address of your TON node. Normally it is the same IP address as your server's external IP. This also can be your proxy server or load balancer IP address. |    yes     |                                                         |
| GCONFURL       | TON mainnet global configuration file.                                                                                                                                              |     no     | https://api.tontech.io/ton/wallet-mainnet.autoconf.json |
| VALIDATOR_PORT | UDP port that must be available from the outside. Used for communication with other nodes.                                                                                          |            |                          43677                          |
| CONSOLE_PORT   | This TCP port is used to access validator's console. Not necessarily to be opened for external access.                                                                              |     no     |                          43678                          |
| LITE_PORT      | Lite-server's TCP port. Used by lite-client.                                                                                                                                        |     no     |                          43679                          |
| LITESERVER     | true or false. Set to true if you want up and running lite-server.                                                                                                                  |     no     |                          false                          |
| STATE_TTL      | Node's state will be gc'd after this time (in seconds).                                                                                                                             |     no     |                          86400                          |
| ARCHIVE_TTL    | Node's archived blocks will be deleted after this time (in seconds).                                                                                                                |     no     |                          86400                          |
| THREADS        | Number of threads used by validator-engine.                                                                                                                                         |     no     |                            8                            |
| VERBOSITY      | Verbosity level.                                                                                                                                                                    |     no     |                            3                            |
| CUSTOM_ARG     | validator-engine might have some undocumented arguments. This is reserved for the test purposes.<br/>For example you can pass **--logname /var/ton-work/log** in order to have log files.|     no     |                                                         |

### Run the node - the quick way
The below command runs docker container with a TON node, that will start synchronization process.

Notice **--network host** option, means that the Docker container will use the network namespace of the host machine.
In this case there is no need to map ports between the host and the container. The container will use the same IP address and ports as the host.
This approach simplifies networking configuration for the container, and usually is used on the dedicated server with assigned public IP.

Keep in mind that this option can also introduce security concerns because the container has access to the host's network interfaces directly, which might not be desirable in a multi-tenant environment.

Check your firewall configuration and make sure that at least UDP port 43677 is publicly available.  
```
docker run -d --name ton-node -v /data/db:/var/ton-work/db \
-e "HOST_IP=<YOUR_PUBLIC_IP>" \
-e "PUBLIC_IP=<YOUR_PUBLIC_IP>" \
-e "LITESERVER=true" \
--network host \
-it ghcr.io/ton-blockchain/ton
```
If you don't need Lite-server, then remove -e "LITESERVER=true".

### Run the node - isolated way
In production environments it is recommended to use **Port mapping** feature of Docker's default bridge network. 
When you use port mapping, Docker allocates a specific port on the host to forward traffic to a port inside the container.
This is ideal for running multiple containers with isolated networks on the same host.
```
docker run -d --name ton-node -v /data/db:/var/ton-work/db \
-e "HOST_IP=<YOUR_PUBLIC_IP>" \
-e "PUBLIC_IP=<YOUR_PUBLIC_IP>" \
-e "VALIDATOR_PORT=443" \
-e "CONSOLE_PORT=88" \
-e "LITE_PORT=443" \
-e "LITESERVER=true" \
-p 443:443/udp \
-p 88:88/tcp \
-p 443:443/tcp \
-it ghcr.io/ton-blockchain/ton
```
Adjust ports per your need. 
Check your firewall configuration and make sure that customized ports (443/udp, 88/tcp and 443/tcp in this example) are publicly available.

### Test if TON node operating correctly
```docker logs ton-node```

This is totally fine if in the log output for some time (up to 30 minutes) you see messages like below:

```log
failed to download proof link: [Error : 651 : no nodes]
```

After some time you should be able to see multiple messages similar to these below:
```log
failed to download key blocks: [Error : 652 : adnl query timeout]
last key block is [ w=-1 s=9223372036854775808 seq=34879845 rcEsfLF3E80PqQPWesW+rlOY2EpXd5UDrW32SzRWgus= C1Hs+q2Vew+WxbGL6PU1P6R2iYUJVJs4032CTS/DQzI= ]
getnextkey: [Error : 651 : not inited]
downloading state (-1,8000000000000000,38585739):9E86E166AE7E24BAA22762766381440C625F47E2B11D72967BB58CE8C90F7EBA:5BFFF759380097DF178325A7151E9C0571C4E452A621441A03A0CECAED970F57: total=1442840576 (71MB/s)downloading state (-1,8000000000000000,38585739):9E86E166AE7E24BAA22762766381440C625F47E2B11D72967BB58CE8C90F7EBA:5BFFF759380097DF178325A7151E9C0571C4E452A621441A03A0CECAED970F57: total=1442840576 (71MB/s)
finished downloading state (-1,8000000000000000,38585739):9E86E166AE7E24BAA22762766381440C625F47E2B11D72967BB58CE8C90F7EBA:5BFFF759380097DF178325A7151E9C0571C4E452A621441A03A0CECAED970F57: total=4520747390
getnextkey: [Error : 651 : not inited]
getnextkey: [Error : 651 : not inited]
```
As you noticed we have mounted docker volume to a local folder **/data/db**. 
Go inside this folder on your server and check if its size is growing (```sudo du -h .*```)

Now connect to the running container:
```
docker exec -ti ton-node /bin/bash
```
and try to connect and execute **getconfig** command via validator-engine-console:
```
validator-engine-console -k client -p server.pub -a localhost:$(jq .control[].port <<< cat /var/ton-work/db/config.json) -c getconfig
```
if you see a json output that means that validator-engine is up, now execute **last** command with a lite-client:
```
lite-client -a localhost:$(jq .liteservers[].port <<< cat /var/ton-work/db/config.json) -p liteserver.pub -c last
```
if you see the following output:
```
conn ready
failed query: [Error : 652 : adnl query timeout]
cannot get server version and time (server too old?)
server version is too old (at least 1.1 with capabilities 1 required), some queries are unavailable
fatal error executing command-line queries, skipping the rest
```
it means that the lite-server is up, but the node is not synchronized yet.
Once the node is syncrhonized, the output of **last** command will be similar to this one:

```
conn ready
server version is 1.1, capabilities 7
server time is 1719306580 (delta 0)
last masterchain block is (-1,8000000000000000,20435927):47A517265B25CE4F2C8B3058D46343C070A4B31C5C37745390CE916C7D1CE1C5:279F9AA88C8146257E6C9B537905238C26E37DC2E627F2B6F1D558CB29A6EC82
server time is 1719306580 (delta 0)
zerostate id set to -1:823F81F306FF02694F935CF5021548E3CE2B86B529812AF6A12148879E95A128:67E20AC184B9E039A62667ACC3F9C00F90F359A76738233379EFA47604980CE8
```
If you can't make it working, refer to the **Troubleshooting** section below.
### Use validator-engine-console
```docker exec -ti ton-node /bin/bash```

```validator-engine-console -k /var/ton-work/keys/client -p /var/ton-work/keys/server.pub -a 127.0.0.1:$(jq .control[].port <<< cat /var/ton-work/db/config.json)```

### Use lite-client
```docker exec -ti ton-node /bin/bash```

```lite-client -p /var/ton-work/keys/liteserver.pub -a 127.0.0.1:$(jq .liteservers[].port <<< cat /var/ton-work/db/config.json)```

If you use lite-client outside the Docker container, copy the **liteserver.pub** from the container:

```docker cp ton-node:/var/ton-work/db/liteserver.pub /your/path```

```lite-client -p /your/path/liteserver.pub -a <PUBLIC_IP>:<LITE_PORT>```

## Kubernetes
todo
## Troubleshooting

### TON node cannot synchronize, constantly see messages [Error : 651 : no nodes] in the log

Get inside the container:

```
docker run -it -v /data/db:/var/ton-work/db \
-e "HOST_IP=<PUBLIC_IP>" \
-e "PUBLIC_IP=<PUBLIC_IP>" \
-e "LITESERVER=true" \
-p 43677:43677/udp \
-p 43678:43678/tcp \
-p 43679:43679/tcp \
--entrypoint /bin/bash \
ghcr.io/ton-blockchain/ton
```
identify your PUBLIC_IP:
```
wget -qO - icanhazip.com
```
compare if resulted IP coincides with your <PUBLIC_IP>. 
If it doesn't, exit container and launch it with the correct public IP.
Then open UDP port (inside the container) you plan to allocate for TON node using netcat utility:
```
nc -ul 43677
```
and from any **other** linux machine check if you can reach this UDP port by sending a test message to that port:
```
other-server> echo "test" | nc -u <PUBLIC_IP> 43677
```
as a result inside the container you have to receive the "test" message.

If you don't get the message inside the docker container, that means that either your firewall, NAT or proxy is blocking it.
Ask your system administrator for assistance. 

### Can't connect to lite-server
* check if lite-server was enabled on start by passing **"LITESERVER=true"** argument;
* check if TCP port (LITE_PORT) is available from the outside. From any other linux machine execute:
 ```
nc -vz <PUBLIC_IP> <LITE_PORT>
```
### How to see what traffic is generated inside the TON docker container?
There is available a traffic monitoring utility inside the container, just execute:
```
iptraf-ng
```
Other tools like **nc**, **wget**, **ifconfig** and **netstat** are also available.

### How to build TON docker image from sources?
```
git clone --recursive https://github.com/ton-blockchain/ton.git
cd ton
docker build .
```
