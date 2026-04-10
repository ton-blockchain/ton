fun main(t: tuple) {
    return t.get(0) is int;
}

/**
@compilation_should_fail
@stderr operator `is` does not work for `unknown`, it works for union types only
 */
