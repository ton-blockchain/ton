fun test(someArr: array<bool>) {
    // it's parsed as `SomeType [a,b,c]`
    return someArr [0, 1, 2];
}

/**
@compilation_should_fail
@stderr unknown type name `someArr`
 */
