fun main() {
    return "hello\k";
}

/**
@compilation_should_fail
@stderr invalid escape sequence \k
 */
