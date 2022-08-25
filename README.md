[![Stack Overflow Group][stack-overflow-badge]][stack-overflow-url]
[![Telegram Foundation Group][telegram-foundation-badge]][telegram-foundation-url]
[![Telegram Community Group][telegram-community-badge]][telegram-community-url]
[![Twitter Group][twitter-badge]][twitter-url]

[telegram-foundation-badge]: https://img.shields.io/badge/-TON%20Foundation-2CA5E0?style=flat&logo=telegram&logoColor=white
[telegram-community-badge]: https://img.shields.io/badge/-TON%20Community-2CA5E0?style=flat&logo=telegram&logoColor=white
[telegram-foundation-url]: https://t.me/tonblockchain
[telegram-community-url]: https://t.me/toncoin
[twitter-badge]: https://img.shields.io/twitter/follow/ton_blockchain
[twitter-url]: https://twitter.com/ton_blockchain
[stack-overflow-badge]: https://img.shields.io/badge/-Stack%20Overflow-FE7A16?style=flat&logo=stack-overflow&logoColor=white
[stack-overflow-url]: https://stackoverflow.com/questions/tagged/ton



# TON

Main TON monorepo, which includes the code of the node/validator, lite-client, tonlib, FunC compiler, etc.

## Updates flow:

* **master branch** - mainnet is running on this stable branch.

    Only emergency updates, urgent updates, or updates that do not affect the main codebase (GitHub workflows / docker images / documentation) are committed directly to this branch.

* **testnet branch** - testnet is running on this branch. The branch contains a set of new updates. After testing, the testnet branch is merged into the master branch and then a new set of updates is added to testnet branch.

* **backlog** - other branches that are candidates to getting into the testnet branch in the next iteration.

Usually, the response to your pull request will indicate which section it falls into.


## "Soft" Pull Request rules

* Thou shall not merge your own PRs, at least one person should review the PR and merge it (4-eyes rule)
* Thou shall make sure that workflows are cleanly completed for your PR before considering merge

## Workflows responsibility
If a CI workflow fails not because of your changes but workflow issues, try to fix it yourself or contact one of the persons listed below via Telegram messenger:

* **C/C++ CI (ccpp-linux.yml)**: TBD
* **C/C++ CI Win64 Compile (ccpp-win64.yml)**: TBD
