# Official TON Docker image

1. [Dockerfile](#docker)
2. [Kubernetes deployment on-premises](#deploy-on-premises-with-metallb-load-balancer-)
3. [Kubernetes deployment on AWS](#deploy-on-aws-cloud-amazon-web-services)
4. [Kubernetes deployment on GCP](#deploy-on-gcp-google-cloud-platform)
5. [Kubernetes deployment on AliCloud](#deploy-on-ali-cloud)
6. [Troubleshooting](#troubleshooting)
## Prerequisites

The TON node, whether it is validator or fullnode, requires a public IP address. 
If your server is within an internal network or kubernetes you have to make sure that the required ports are available from the outside.

Also pay attention at [hardware requirements](https://docs.ton.org/participate/run-nodes/full-node) for TON fullnodes and validators. Pods and StatefulSets in this guide imply these requirements. 

It is recommended to everyone to read Docker chapter first in order to get a better understanding about TON Docker image and its parameters.  

## Docker

### Installation
```docker pull ghcr.io/ton-blockchain/ton:latest```

### Configuration
TON validator-engine supports number of command line parameters, 
these parameters can be handed over to the container via environment variables. 
Below is the list of supported arguments and their default values:

| Argument          | Description                                                                                                                                                                               | Mandatory? |                      Default value                      |
|:------------------|:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:----------:|:-------------------------------------------------------:|
| PUBLIC_IP         | This will be a public IP address of your TON node. Normally it is the same IP address as your server's external IP. This also can be your proxy server or load balancer IP address.       |    yes     |                                                         |
| GLOBAL_CONFIG_URL | TON global configuration file. Mainnet - https://ton.org/global-config.json, Testnet - https://ton.org/testnet-global.config.json                                                         |     no     | https://api.tontech.io/ton/wallet-mainnet.autoconf.json |
| DUMP_URL          | URL to TON dump. Specify dump from https://dump.ton.org. If you are using testnet dump, make sure to download global config for testnet.                                                  |     no     |                                                         |
| VALIDATOR_PORT    | UDP port that must be available from the outside. Used for communication with other nodes.                                                                                                |     no     |                          30001                          |
| CONSOLE_PORT      | This TCP port is used to access validator's console. Not necessarily to be opened for external access.                                                                                    |     no     |                          30002                          |
| LITE_PORT         | Lite-server's TCP port. Used by lite-client.                                                                                                                                              |     no     |                          30003                          |
| LITESERVER        | true or false. Set to true if you want up and running lite-server.                                                                                                                        |     no     |                          false                          |
| STATE_TTL         | Node's state will be gc'd after this time (in seconds).                                                                                                                                   |     no     |                          86400                          |
| ARCHIVE_TTL       | Node's archived blocks will be deleted after this time (in seconds).                                                                                                                      |     no     |                          86400                          |
| THREADS           | Number of threads used by validator-engine.                                                                                                                                               |     no     |                            8                            |
| VERBOSITY         | Verbosity level.                                                                                                                                                                          |     no     |                            3                            |
| CUSTOM_ARG        | validator-engine might have some undocumented arguments. This is reserved for the test purposes.<br/>For example you can pass **--logname /var/ton-work/log** in order to have log files. |     no     |                                                         |

### Run the node - the quick way
The below command runs docker container with a TON node, that will start synchronization process.

Notice **--network host** option, means that the Docker container will use the network namespace of the host machine.
In this case there is no need to map ports between the host and the container. The container will use the same IP address and ports as the host.
This approach simplifies networking configuration for the container, and usually is used on the dedicated server with assigned public IP.

Keep in mind that this option can also introduce security concerns because the container has access to the host's network interfaces directly, which might not be desirable in a multi-tenant environment.

Check your firewall configuration and make sure that at least UDP port 43677 is publicly available.
Find out your PUBLIC_IP:
```
curl -4 ifconfig.me
```
and replace it in the command below:
```
docker run -d --name ton-node -v /data/db:/var/ton-work/db \
-e "PUBLIC_IP=<PUBLIC_IP>" \
-e "LITESERVER=true" \
-e "DUMP_URL=https://dump.ton.org/dumps/latest.tar.lz" \
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
-e "PUBLIC_IP=<PUBLIC_IP>" \
-e "DUMP_URL=https://dump.ton.org/dumps/latest.tar.lz" \
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

### Verify if TON node is operating correctly
After executing above command check the log files:

```docker logs ton-node```

This is totally fine if in the log output for some time (up to 15 minutes) you see messages like:

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
Once the node is synchronized, the output of **last** command will be similar to this one:

```
conn ready
server version is 1.1, capabilities 7
server time is 1719306580 (delta 0)
last masterchain block is (-1,8000000000000000,20435927):47A517265B25CE4F2C8B3058D46343C070A4B31C5C37745390CE916C7D1CE1C5:279F9AA88C8146257E6C9B537905238C26E37DC2E627F2B6F1D558CB29A6EC82
server time is 1719306580 (delta 0)
zerostate id set to -1:823F81F306FF02694F935CF5021548E3CE2B86B529812AF6A12148879E95A128:67E20AC184B9E039A62667ACC3F9C00F90F359A76738233379EFA47604980CE8
```
If you can't make it working, refer to the [Troubleshooting](#troubleshooting) section below.
### Use validator-engine-console
```docker exec -ti ton-node /bin/bash```

```validator-engine-console -k client -p server.pub -a 127.0.0.1:$(jq .control[].port <<< cat /var/ton-work/db/config.json)```

### Use lite-client
```docker exec -ti ton-node /bin/bash```

```lite-client -p liteserver.pub -a 127.0.0.1:$(jq .liteservers[].port <<< cat /var/ton-work/db/config.json)```

If you use lite-client outside the Docker container, copy the **liteserver.pub** from the container:

```docker cp ton-node:/var/ton-work/db/liteserver.pub /your/path```

```lite-client -p /your/path/liteserver.pub -a <PUBLIC_IP>:<LITE_PORT>```

### Stop TON docker container
```
docker stop ton-node
```

## Kubernetes
### Deploy in a quick way (without load balancer)
If the nodes within your kubernetes cluster have external IPs, 
make sure that the PUBLIC_IP used for validator-engine matches the node's external IP.
If all Kubernetes nodes are inside DMZ - skip this section.

#### Prepare
If you are using **flannel** network driver you can find node's IP this way: 
```yaml
kubectl get nodes
kubectl describe node <NODE_NAME> | grep public-ip
```
for **calico** driver use:
```yaml
kubectl describe node <NODE_NAME> | grep IPv4Address
```
Double check if your Kubernetes node's external IP coincides with the host's IP address:
```
kubectl run --image=ghcr.io/ton-blockchain/ton:latest validator-engine-pod --env="HOST_IP=1.1.1.1" --env="PUBLIC_IP=1.1.1.1"
kubectl exec -it validator-engine-pod -- curl -4 ifconfig.me
kubectl delete pod validator-engine-pod
```
If IPs do not match, refer to the sections where load balancers are used.

Now do the following:
* Add a label to this particular node. 
* By this label our pod will know where to be deployed and what storage to use:  
```
kubectl label nodes <NODE_NAME> node_type=ton-validator
```
* Replace **<PUBLIC_IP>** (and ports if needed) in file [ton-node-port.yaml](ton-node-port.yaml).
* Replace **<LOCAL_STORAGE_PATH>** with a real path on host for Persistent Volume.
* If you change the ports, make sure you specify appropriate env vars in Pod section.
* If you want to use dynamic storage provisioning via volumeClaimTemplates, feel free to create own StorageClass. 

#### Install
```yaml
kubectl apply -f ton-node-port.yaml
```

this deployment uses host's network stack (**hostNetwork: true**) option and service of **NodePort** type.
Actually you can also use service of type **LoadBalancer**.
This way the service will get public IP assigned to the endpoints.

#### Verify installation
See if service endpoints were correctly created:

```yaml
kubectl get endpoints

NAME                   ENDPOINTS
validator-engine-srv   <PUBLIC_IP>:30002,<PUBLIC_IP>:30001,<PUBLIC_IP>:30003
```
Check the logs for the deployment status:
```yaml
kubectl logs validator-engine-pod
```
or go inside the pod and check if blockchain size is growing:
```yaml
kubectl exec --stdin --tty validator-engine-pod -- /bin/bash
du -h .
```
### Deploy on-premises with metalLB load balancer 

Often Kubernetes cluster is located in DMZ, is behind corporate firewall and access is controlled via proxy configuration.
In this case we can't use  host's network stack (**hostNetwork: true**) within a Pod and must manually proxy the access to the pod.

A **LoadBalancer** service type automatically provisions an external load balancer (such as those provided by cloud providers like AWS, GCP, Azure) and assigns a public IP address to your service. In a non-cloud environment or in a DMZ setup, you need to manually configure the load balancer.

If you are running your Kubernetes cluster on-premises or in an environment where an external load balancer is not automatically provided, you can use a load balancer implementation like MetalLB.

#### Prepare
Select the node where persistent storage will be located for TON validator.
* Add a label to this particular node. By this label our pod will know where to be deployed:
```
kubectl label nodes <NODE_NAME> node_type=ton-validator
```
* Replace **<PUBLIC_IP>** (and ports if needed) in file [ton-metal-lb.yaml](ton-metal-lb.yaml).
* Replace **<LOCAL_STORAGE_PATH>** with a real path on host for Persistent Volume.
* If you change the ports, make sure you specify appropriate env vars in Pod section.
* If you want to use dynamic storage provisioning via volumeClaimTemplates, feel free to create own StorageClass.

* Install MetalLB
```yaml
kubectl apply -f https://raw.githubusercontent.com/metallb/metallb/v0.14.5/config/manifests/metallb-native.yaml
```

* Configure MetalLB
Create a configuration map to define the IP address range that MetalLB can use for external load balancer services.
```yaml
apiVersion: metallb.io/v1beta1
kind: IPAddressPool
metadata:
  name: first-pool
  namespace: metallb-system
spec:
  addresses:
    - 10.244.1.0/24 <-- your CIDR address
```
apply configuration
```yaml
kubectl apply -f metallb-config.yaml
```
#### Install

```yaml
kubectl apply -f ton-metal-lb.yaml
```
We do not use Pod Node Affinity here, since the Pod will remember the host with local storage it was bound to.

#### Verify installation
Assume your network CIDR (**--pod-network-cidr**) within cluster is 10.244.1.0/24, then you can compare the output with the one below:
```yaml
kubectl get service

NAME                   TYPE           CLUSTER-IP       EXTERNAL-IP   PORT(S)                                           AGE
kubernetes             ClusterIP      <NOT_IMPORTANT>  <none>        443/TCP                                           28h
validator-engine-srv   LoadBalancer   <NOT_IMPORTANT>  10.244.1.1    30001:30001/UDP,30002:30002/TCP,30003:30003/TCP   60m
```
you can see that endpoints are pointing to metal-LB subnet:
```
kubectl get endpoints

NAME                   ENDPOINTS
kubernetes             <IP>:6443
validator-engine-srv   10.244.1.10:30002,10.244.1.10:30001,10.244.1.10:30003
```
and metal-LB itself operates with the right endpoint:
```
kubectl describe service metallb-webhook-service -n metallb-system

Name:              metallb-webhook-service
Namespace:         metallb-system
Selector:          component=controller
Type:              ClusterIP
IP:                <NOT_IMPORTANT_IP>
IPs:               <NOT_IMPORTANT_IP>
Port:              <unset>  443/TCP
TargetPort:        9443/TCP
Endpoints:         10.244.2.3:9443  <-- CIDR
```

Use the commands from the previous chapter to see if node operates properly.

### Deploy on AWS cloud (Amazon Web Services)

#### Prepare
* AWS EKS is configured with worker nodes with selected add-ons:
  * CoreDNS - Enable service discovery within your cluster.
  * kube-proxy - Enable service networking within your cluster.
  * Amazon VPC CNI - Enable pod networking within your cluster.
* Allocate Elastic IP.
* Replace **<PUBLIC_IP>** with the newly created Elastic IP in [ton-aws.yaml](ton-aws.yaml)
* Replace **<ELASTIC_IP_ID>** with Elastic IP allocation ID (see in AWS console).
* Adjust StorageClass name. Make sure you are providing fast storage.

#### Install

```kubectl apply -f ton-aws.yaml```

#### Verify installation
Use instructions from the previous sections. 

### Deploy on GCP (Google Cloud Platform)

#### Prepare
* Kubernetes cluster of type Standard (not Autopilot).
* Premium static IP address. 
* Adjust firewall rules and security groups to allow ports 30001/udp, 30002/tcp and 30003/tcp (default ones).
* Replace **<PUBLIC_IP>** (and ports if needed) in file [ton-gcp.yaml](ton-gcp.yaml).
* Adjust StorageClass name. Make sure you are providing fast storage.

* Load Balancer will be created automatically according to Kubernetes service in yaml file.

#### Install
```kubectl apply -f ton-gcp.yaml```

#### Verify installation
Use instructions from the previous sections.

### Deploy on Ali Cloud

#### Prepare
* AliCloud kubernetes cluster.
* Elastic IP.
* Replace **<ELASTIC_IP_ID>** with Elastic IP allocation ID (see in AliCloud console).
* Replace **<PUBLIC_IP>** (and ports if needed) in file [ton-ali.yaml](ton-ali.yaml) with the elastic IP attached to your CLB.
* Adjust StorageClass name. Make sure you are providing fast storage.

#### Install
```kubectl apply -f ton-ali.yaml```

As a result CLB (classic internal Load Balancer) will be created automatically with assigned external IP.

#### Verify installation
Use instructions from the previous sections.

## Troubleshooting
## Docker 
### TON node cannot synchronize, constantly see messages [Error : 651 : no nodes] in the log

Start the new container without starting validator-engine:

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
curl -4 ifconfig.me
```
compare if resulted IP coincides with your <PUBLIC_IP>. 
If it doesn't, exit container and launch it with the correct public IP.
Then open UDP port (inside the container) you plan to allocate for TON node using netcat utility:
```
nc -ul 30001
```
and from any **other** linux machine check if you can reach this UDP port by sending a test message to that port:
```
echo "test" | nc -u <PUBLIC_IP> 30001
```
as a result inside the container you have to receive the "test" message.

If you don't get the message inside the docker container, that means that either your firewall, LoadBalancer, NAT or proxy is blocking it.
Ask your system administrator for assistance. 

In the same way you can check if TCP port is available:

Execute inside the container ```nc -l 30003``` and test connection from another server
```nc -vz <PUBLIC_IP> 30003```

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
Other tools like **tcpdump**, **nc**, **wget**, **curl**, **ifconfig**, **pv**, **plzip**, **jq** and **netstat** are also available.

### How to build TON docker image from sources?
```
git clone --recursive https://github.com/ton-blockchain/ton.git
cd ton
docker build .
```

## Kubernetes
### AWS
#### After installing AWS LB, load balancer is still not available (pending):
```
kubectl get deployment -n kube-system aws-load-balancer-controller
```
Solution:

Try to install AWS LoadBalancer using ```Helm``` way.

---

#### After installing AWS LB and running ton node, service shows error:

```k describe service validator-engine-srv```

```log
Failed build model due to unable to resolve at least one subnet (0 match VPC and tags: [kubernetes.io/role/elb])
```
Solution:

You haven't labeled the AWS subnets with the correct resource tags.

* Public Subnets should be resource tagged with: kubernetes.io/role/elb: 1
* Private Subnets should be tagged with: kubernetes.io/role/internal-elb: 1
* Both private and public subnets should be tagged with: kubernetes.io/cluster/${your-cluster-name}: owned
* or if the subnets are also used by non-EKS resources kubernetes.io/cluster/${your-cluster-name}: shared

So create tags for at least one subnet:
```
kubernetes.io/role/elb: 1
kubernetes.io/cluster/<YOUR_CLUSTER_NAME>: owner
```
---
#### AWS Load Balancer works, but I still see ```[no nodes]``` in validator's log
It is required to add the security group for the EC2 instances to the load balancer along with the default security group. 
It's a misleading that the default security group has "everything open."

Add security group (default name is usually something like 'launch-wizard-1').
And make sure you allow the ports you specified or default ports 30001/udp, 30002/tcp and 30003/tcp.

You can also set inbound and outbound rules of new security group to allow ALL ports and for ALL protocols and for source CIDR 0.0.0.0/0 for testing purposes.

---

#### Pending PersistentVolumeClaim ```Waiting for a volume to be created either by the external provisioner 'ebs.csi.aws.com' or manually by the system administrator.```

Solution: 

Configure Amazon EBS CSI driver for working PersistentVolumes in EKS.

1. Enable IAM OIDC provider
```
eksctl utils associate-iam-oidc-provider --region=us-west-2 --cluster=k8s-my --approve
```

2. Create Amazon EBS CSI driver IAM role
```
eksctl create iamserviceaccount \
--region us-west-2 \
--name ebs-csi-controller-sa \
--namespace kube-system \
--cluster k8s-my \
--attach-policy-arn arn:aws:iam::aws:policy/service-role/AmazonEBSCSIDriverPolicy \
--approve \
--role-only \
--role-name AmazonEKS_EBS_CSI_DriverRole
```

3. Add the Amazon EBS CSI add-on
```yaml
eksctl create addon --name aws-ebs-csi-driver --cluster k8s-my --service-account-role-arn arn:aws:iam::$(aws sts get-caller-identity --query Account --output text):role/AmazonEKS_EBS_CSI_DriverRole --force
```
### Google Cloud
#### Load Balancer cannot obtain external IP (pending)

```
kubectl describe service validator-engine-srv

Events:
Type     Reason                                 Age                  From                Message
  ----     ------                                 ----                 ----                -------
Warning  LoadBalancerMixedProtocolNotSupported  7m8s                 g-cloudprovider     LoadBalancers with multiple protocols are not supported.
Normal   EnsuringLoadBalancer                   113s (x7 over 7m8s)  service-controller  Ensuring load balancer
Warning  SyncLoadBalancerFailed                 113s (x7 over 7m8s)  service-controller  Error syncing load balancer: failed to ensure load balancer: mixed protocol is not supported for LoadBalancer
```
Solution:

Create static IP address of type Premium in GCP console and use it as a value for field ```loadBalancerIP``` in Kubernetes service.

### Ali Cloud

#### Validator logs always show
```
Client got error [PosixError : Connection reset by peer : 104 : Error on [fd:45]]
[!NetworkManager][&ADNL_WARNING]  [networkmanager]: received too small proxy packet of size 21
```
Solution:

The node is sychnronizing, but very slow though.
Try to use Network Load Balancer (NLB) instead of default CLB.


