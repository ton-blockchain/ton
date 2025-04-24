fun cantAssignIntNToCoins(n: int8, c: coins) {
    n = c as int8;      // ok
    n = c as int;       // ok
    n = c;
}

/**
@compilation_should_fail
@stderr can not assign `coins` to variable of type `int8`
@stderr n = c;
 */
