struct A {
    v1: int = "asdf";
}

/**
@compilation_should_fail
@stderr can not assign `string` to `int`
@stderr v1: int = "asdf"
 */
