<div align="center">
  <a href="https://ton.org">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://ton.org/download/ton_logo_dark_background.svg">
      <img alt="TON logo" src="https://ton.org/download/ton_logo_light_background.svg">
    </picture>
  </a>
  <h3>Reference implementation of TON Node and tools</h3>
  <hr/>
</div>

## 
[![TON Overflow Group][ton-overflow-badge]][ton-overflow-url]
[![Stack Overflow Group][stack-overflow-badge]][stack-overflow-url]
[![Telegram Community Chat][telegram-tondev-badge]][telegram-tondev-url]
[![Telegram Community Group][telegram-community-badge]][telegram-community-url]
[![Telegram Foundation Group][telegram-foundation-badge]][telegram-foundation-url]
[![Twitter Group][twitter-badge]][twitter-url]

[telegram-foundation-badge]: https://img.shields.io/badge/TON%20Foundation-2CA5E0?logo=telegram&logoColor=white&style=flat
[telegram-community-badge]: https://img.shields.io/badge/TON%20Community-2CA5E0?logo=telegram&logoColor=white&style=flat
[telegram-tondev-badge]: https://img.shields.io/badge/chat-TONDev-2CA5E0?logo=telegram&logoColor=white&style=flat
[telegram-foundation-url]: https://t.me/tonblockchain
[telegram-community-url]: https://t.me/toncoin
[telegram-tondev-url]: https://t.me/tondev_eng
[twitter-badge]: https://img.shields.io/twitter/follow/ton_blockchain
[twitter-url]: https://twitter.com/ton_blockchain
[stack-overflow-badge]: https://img.shields.io/badge/-Stack%20Overflow-FE7A16?style=flat&logo=stack-overflow&logoColor=white
[stack-overflow-url]: https://stackoverflow.com/questions/tagged/ton
[ton-overflow-badge]: https://img.shields.io/badge/-TON%20Overflow-FE7A16?style=flat&logo=stack-overflow&logoColor=white
[ton-overflow-url]: https://answers.ton.org



Main TON monorepo, which includes the code of the node/validator, lite-client, tonlib, FunC compiler, etc.

## The Open Network

__The Open Network (TON)__ is a fast, secure, scalable blockchain focused on handling _millions of transactions per second_ (TPS) with the goal of reaching hundreds of millions of blockchain users.
- To learn more about different aspects of TON blockchain and its underlying ecosystem check [documentation](https://docs.ton.org/)
- To run node, validator or lite-server check [Participate section](https://docs.ton.org/participate/nodes/node-types)
- To develop decentralised apps check [Tutorials](https://docs.ton.org/develop/smart-contracts/), [FunC docs](https://docs.ton.org/develop/func/overview) and [DApp tutorials](https://docs.ton.org/develop/getting-started#web-and-dapps-development)
- To work on TON check [wallets](https://ton.app/wallets), [explorers](https://ton.app/explorers), [DEXes](https://ton.app/dex) and [utilities](https://ton.app/utilities)
- To interact with TON check [APIs](https://docs.ton.org/develop/dapps/apis)

## Updates flow:

* **master branch** - mainnet is running on this stable branch.

    Only emergency updates, urgent updates, or updates that do not affect the main codebase (GitHub workflows / docker images / documentation) are committed directly to this branch.

* **testnet branch** - testnet is running on this branch. The branch contains a set of new updates. After testing, the testnet branch is merged into the master branch and then a new set of updates is added to testnet branch.

* **backlog** - other branches that are candidates to getting into the testnet branch in the next iteration.

Usually, the response to your pull request will indicate which section it falls into.


## "Soft" Pull Request rules

* Thou shall not merge your own PRs, at least one person should review the PR and merge it (4-eyes rule)
* Thou shall make sure that workflows are cleanly completed for your PR before considering merge

## Installation of TON artifacts
### deb (apt)

```sh
sudo bash -c 'echo "deb [trusted=yes] https://github.com/ton-blockchain/ton/releases/latest/download ./" > /etc/apt/sources.list.d/10-ton.list'
sudo apt update
sudo apt install ton
```

### brew

```sh
brew tap ton-blockchain/ton
brew install ton
```
