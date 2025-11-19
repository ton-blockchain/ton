struct A {
    v1: int = "asdf";
}

/**
@compilation_should_fail
@stderr can not assign `slice` to `int`
@stderr v1: int = "asdf"
 */
