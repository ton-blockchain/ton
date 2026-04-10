fun invalidNever(): never {
    if (random.uint256()) { throw 123; }
}

/**
@compilation_should_fail
@stderr a function returning `never` can not have a reachable endpoint
 */
