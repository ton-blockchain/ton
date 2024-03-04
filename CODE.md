# Code Overview

## Alphabetical Overview

Just use the github/IDE file-tree to open a folders and review the related README.md:

* https://github.com/ton-blockchain/ton/tree/master/adnl

## Subsystems Overview

Ideally, the folder-structure of the source-tree would provide the hierachical/architectural Subsystems Overview.

This is (for different reasons) not always possible. Find below a manually recreated overview:

* system
  * [TON Node Docker](docker)
* executable
  * [blockchain-explorer](blockchain-explorer)
  * [dht-server](dht-server)
    * dth*
  * [http](http)
  * [lite-client](lite-client)  
  * [rldp-http-proxy](rldp-http-proxy)
    * rldp*
  * [storage](storage)
  * [tdactor](tdactor)
    * td*
  * [terminal](terminal)
  * [validator](validator)
    * validator-*
* libraries
  * [adnl](adnl)
  * [catchain](catchain)
  * [common](common)
  * [create-hardfork](create-hardfork) 
  * [crypto](crypto)  
  * [emulator](emulator)
  * [fec](fec)
  * [keyring](keyring)
  * [keys](keys)
  * [overlay](overlay)
  * [ton](ton) 
  * [tonlib](tonlib)
    * [example](example)
  * [utils](utils)
* compilation
  * [CMake](CMake)
  * [assembly](assembly)
  * [memprof](memprof)
  * [test](test)
  * [tl](tl)
    * tl*
* other
  * [doc](doc)
  * [third-party](third-party)  




