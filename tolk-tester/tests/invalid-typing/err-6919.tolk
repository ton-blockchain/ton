struct Gen<T> {
    price: coins = (1 as T) + "",
}

fun main() {
    var c: Gen<int8> = {};
    return c;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int8` and `slice`
 */
