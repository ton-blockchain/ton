# Manual Registration DNS smart contract

## Overview

Current directory contains the FuC code for DNS smartcontract implementation and tools for working with FunC/Fift. Based on [TON.pdf#4.3](https://test.ton.org/ton.pdf#subsection.4.3)  
DOMAIN_MAX_SIZE is `122 bytes + 1 byte space = 122 letters`

## HowTo

The scripts require to have FunC compiler and Fift assember accessible into env PATH variable.

### DNS scm tools

* `./dns_scm.sh` - base tool for work with current SCM. Wraps `./compile` with the most useful operations (edit file for pointng another constants), use help
* `./dns_tests.sh` - runs integration tests for current implementation, use help

### General tools (can be used with custom smart contracts)

For making development more convenient you may use following bash scripts.

* `./compile` - generates BOC file from FunC code, use help
* `./test` - invokes FunC code into VM with inbound binary message, use help


#### Examples

Register domain
```
$ ./dns_scm.sh register -d "telegram.org" -c -1 -r -1:e4ee276edda93e6b6e9aa1951db9c7e33c1f09127383550bab2367fc471a4696 --seqno 4
```
is the same as
```
./compile -o output/1 -k output/scm.pk dns.fc -a output/dns.addr -m messages/manual-register.fif.sh -d telegram.org -c -1 -r -1:e4ee276edda93e6b6e9aa1951db9c7e33c1f09127383550bab2367fc471a4696 --seqno 4
```

check `./dns_scm.sh --help` for getting more info

### Elements of DNS
Based on [RFC1034](https://tools.ietf.org/html/rfc1034)  

* ZONE
```
*.domain
*.my.domain
*.its.my.domain
```
* NAME SERVICE(NS) - stores information about **ZONES**  
  represented as an address of smartcontract, also works as [RESOLVER](https://tools.ietf.org/html/rfc1034#section-2.4)  
  Current scm creates one **NS**, child **NS** is stored in tree with `category=-1`

* RESOURCE RECORD(RR), [RFC1034](https://tools.ietf.org/html/rfc1034#section-3.6)  
  There is 2 types of **RR** - **NS** and **A**(Alias, DNSRecord)  
  **NS** is stored with `category=-1` into the registry

### Data

There is a DNS smartcontract data block description (**c4 registry**)  
```
                                                                                    \
 _______________________________________________________________                     |
| seqno: 32-bit |  public_key: 256-bit                          |                    |
|_______________|_______________________________________________|                    |
            | registry: PfxHashmap 984              |                                |
            |       (Hashmap 16 ResourceRecord)     |                                |
            |_______________________________________|                                |
            /      |         |                                                       |
           /       |         | "com\0"                                               |-- Root NS
         ...      ...   _____|____________________________                           |
                       | Zone: Hashmap 16 ^ResourceRecord |                          |
                       |__________________________________|                          |
                   -1  /               | 1           |     \                         |
                   ___/____        ____|_____        |      \                        |
                  | RR: NS |      | RR: A    |      ...     ...                      |
                  |________|      |__________|                                       |
                                                                                     |
                                                                                    /
```

**RecourseRecord (RR)** is stored as
```
                                 \
    ______________________        |
   | expire_at: uint32    |       |
   |______________________|       |
    / ref1                        |-- RR
 __/__________                    |
| Recourse    |                   |
|_____________|                   |
                                 /
```

**ResourceRecord TL-B**

```
resource_scm$001
  workchain_id:int8 address:bits256 = Resource;

resource_record$000
  resource: Resource
  ttl: uint32 = ResourceRecord;
```

## External Messages

All external messages contains 256-bits signature and then message body

```
_ _: MessageUpgrade = MessageBody;
_ _: MessageDropData = MessageBody;
_ _: MessageAddResourceRecord = MessageBody;
_ _: MessageTransferSCM = MessageBody;

ext_message$_
  signature: uint512
  message_body: MessageBody
```

### External: Upgrade TL-B

Current message is responsible for upgrading **code** of smartcontract

```
upgrade_action$0001: ActionUpgrade
ext_message_body$_
  seqno: uint32
  action: ActionUpgrade
  code: ^Cell = MessageUpgrade;
```

### External: DropData TL-B (For development purposes only)

Current message is flush c4 register of the smartcontract

```
upgrade_action$0010: ActionDropData
ext_message_body$_
  seqno: uint32
  action: ActionDropData = MessageDropData
```

### External: Add ResourceRecord TL-B

```
action_add_rr$0011 = ActionAddResourceRecord;
ext_message_body$_
  seqno: uint32
  action: ActionAddResourceRecord
  category: int16
  domain: uint32
  resource_record: ^(ResourceRecord) = MessageAddResourceRecord;
```

### External: Transfer scm TL-B

```
action_add_rr$0100 = MessageTransferSCM;

ext_message$_
  seqno: uint32
  action: ActionTransferSCM
  public_key: (^uint256) = MessageTransferSCM;
```

## Error codes

Code | Message
--- | ---
33 | Wrong seqno
34 | Wrong signature
104 | Unsupported Resource type
105 | Invalid symbol in domain name

## Peformance

```
./dns-test.sh --perf
```
