contract BadDirective {
    incomingMessages: int
}

fun main() {
    return 0;
}

/**
@compilation_should_fail
@stderr `incomingMessages` can not be serialized: type `int`
@stderr because type `int` is not serializable
 */
