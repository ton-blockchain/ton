enum NumAmounts: coins {
    Neg = 100,
    Zero = 200,
    Pos = 100000000,
}

fun main(): map <NumAmounts, int32> {
    return [];
}

/**
@compilation_should_fail
@stderr invalid `map`: type `NumAmounts` can not be used as a key
 */
