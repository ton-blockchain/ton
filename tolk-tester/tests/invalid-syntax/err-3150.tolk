struct A {
    id: int = 3;
}

/**
@compilation_should_fail
@stderr default values for fields not supported yet
@stderr id: int = 3
*/
