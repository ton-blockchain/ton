struct A {
    id: int;
}

fun main() {
    A { id: 1, id: 2 };
}

/**
@compilation_should_fail
@stderr duplicate field initialization
*/
