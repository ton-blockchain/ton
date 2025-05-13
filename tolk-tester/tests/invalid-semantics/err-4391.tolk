struct User {
    id: int;
    name: slice;
}

fun main() {
    var u: User = { id: 1 };
}

/**
@compilation_should_fail
@stderr field `name` missed in initialization of struct `User`
 */
