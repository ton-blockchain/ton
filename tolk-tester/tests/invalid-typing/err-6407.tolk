fun invalidNever(): never {
    if (random()) { throw 123; }
}

/**
@compilation_should_fail
@stderr a function returning `never` can not have a reachable endpoint
 */
