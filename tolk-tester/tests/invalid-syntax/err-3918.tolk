const BAD = """abc\""";

fun main() {
    return 0;
}

/**
@compilation_should_fail
@stderr string pasts past end of file
 */
