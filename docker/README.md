# TON Node Docker Image

Using docker to run The Open Network Node.

### Install Image

This command pulls (installs) the latest ton docker image to your local machine.

```sh
docker pull ghcr.io/ton-blockchain/ton:latest
```

### Build Image

You can build a (customized or not) ton docker image from source code on your local machine.

```sh
docker build -t my/ton .
```

You can then use `my/ton` instead of `ton-blockchain/ton:latest` in the commands below.


### Create Volume
```sh
docker volume create ton-db
```

### Run
```sh
docker run -d --name ton-node --mount source=ton-db,target=/var/ton-work/db --network host -e "PUBLIC_IP=<YOUR_PUBLIC_IP>" -e "CONSOLE_PORT=<TCP-PORT1>" -e "LITESERVER=true" -e "LITE_PORT=<TCP-PORT2>" -it ghcr.io/ton-blockchain/ton
```


If you don't need Liteserver, then remove -e "LITESERVER=true".

### Use
```sh
docker exec -ti <container-id> /bin/bash
```

```sh
./validator-engine-console -k client -p server.pub -a <IP>:<TCP-PORT1>
```

IP:PORT is shown at start of container.

### Lite-Client
To use lite-client you need to get liteserver.pub from container.

```sh
docker cp <container-id>:/var/ton-work/db/liteserver.pub /your/path
```

Then you can connect to it, but be sure you use right port, it's different from fullnode console port.

```sh
lite-client -a <IP>:<TCP-PORT2> -p liteserver.pub
```
