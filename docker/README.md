# The Open Network Node
Dockerfile for The Open Network Node

#### Install
```docker pull ghcr.io/ton-blockchain/ton:latest```
#### Create volume
```docker volume create ton-db```
#### Run
```docker run -d --name ton-node --mount source=ton-db,target=/var/ton-work/db --network host -e "PUBLIC_IP=<YOUR_PUBLIC_IP>" -e "CONSOLE_PORT=<TCP-PORT1>" -e "LITESERVER=true" -e "LITE_PORT=<TCP-PORT2>" -it ghcr.io/ton-blockchain/ton```


If you don't need Liteserver, then remove -e "LITESERVER=true".

#### Use
```docker exec -ti <container-id> /bin/bash```

```./validator-engine-console -k client -p server.pub -a <IP>:<TCP-PORT1>```

IP:PORT is shown at start of container.

#### Lite-client
To use lite-client you need to get liteserver.pub from container.

```docker cp <container-id>:/var/ton-work/db/liteserver.pub /your/path```

Then you can connect to it, but be sure you use right port, it's different from fullnode console port.

```lite-client -a <IP>:<TCP-PORT2> -p liteserver.pub```
