type BadCoins = coins
type BadMap = map<BadCoins, int32>;

fun main(items: array<BadMap>, key: coins): bool {
    return items.get(0).exists(key);
}

/**
@compilation_should_fail
@stderr invalid `map`: type `BadCoins` can not be used as a key
@stderr array<BadMap>
 */
