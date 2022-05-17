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
