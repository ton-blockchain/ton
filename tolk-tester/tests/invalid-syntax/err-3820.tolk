fun main() {
    return "hello\k";
}

/**
@compilation_should_fail
@stderr unknown escape sequence \k
 */
