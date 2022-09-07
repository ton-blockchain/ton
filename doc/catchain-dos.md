# Catchain DoS protection
## Consensus-safe upgrade
Since catchain protocol describes the way validators come to consensus, it should be updated in simultaneous manner to ensure that at any moment of time at least 2/3 of validators (measured by weight) are using the same version of the protocol. Innate way to do it in TON is to use Configuration parameters, when absence or old version of some configuration parameter corresponds to the old behavior, while new version controls updated catchain protocol behavior.

That way, update of catchain protocol should be done in the following way: each validator upgrades its node software and votes for a new config parameter. This method ensures that switching to updated catchain protocol will happen only after 3/4 (more than 2/3) of validators upgrade.

## Covering block dependencies by hash
Catchain protocol version is controlled by `ConfigParam 29`: `proto_version` field in constructors `consensus_config_v3` and older (for other constructors `proto_version` is equal to `0`).

For catchain protocol version higher or equal to 2 hash covers catchain block dependencies, that prevents relaying of blocks with altered dependencies.

## Limiting maximal catchain block height and size
### Catchain block size
All catchain messages, except `REJECT` message, have (and had) a limited size. After update `REJECT` message will be limited to 1024 bytes. At the same time one block contains at most number of block candidates per round messages. That way, 16kb limit per catchain block should be enough to prevent DoS.
### Limiting block height
Another potential DoS is related to a situation when a malbehaviouring node sends too many catchain blocks. Note that limiting of maximal number of blocks per second is not a good solution since it will hinder synchronization after node disconnect.
At the same time, catchain groups exist for quite short period of time (around a few hunderd seconds), while number of blocks production is determined by "natural block production speed" on the one hand and number of blocks generated to decrease dependencies size on the other. In any case, total number of blocks is limited by `catchain_lifetime * natural_block_production_speed * (1 + number_of_catchain_participants / max_dependencies_size)`.
To prevent DoS attack we limit maximal height of the block which will be processed by node by `catchain_lifetime * natural_block_production_speed * (1 + number_of_catchain_participants / max_dependencies_size)`, where `catchain_lifetime` is set by `ConfigParam 28` (`CatchainConfig`), `natural_block_production_speed` and `max_dependencies_size` are set by `ConfigParam 29` (`ConsensusConfig`) (`natural_block_production_speed` is calculated as `catchain_max_blocks_coeff / 1000`) and `number_of_catchain_participants` is set from catchain group configuration.

By default, `catchain_max_blocks_coeff` is set to zero: special value which means that there is no limitation on catchain block height. It is recommended to set `catchain_max_blocks_coeff` to `10000`: we expect that natural production rate is about one block per 3 seconds, so we set the coefficient to allow 30 times higher block production than expected. At the same time, this number is low enough to prevent resource-intensive attacks.

To prevent DoS with many blocks of the same height, nodes will ignore any blocks from a "bad" node (i.e. one which created a fork) unless the new block is required by some other block from a "good" node.
