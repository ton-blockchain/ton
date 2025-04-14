
fun main() {
    match (0 as int | slice) {
        int => 1,
        else => 10,
    };
}

/**
@compilation_should_fail
@stderr `else` is not allowed in `match` by type; you should cover all possible types
@stderr else => 10
 */
