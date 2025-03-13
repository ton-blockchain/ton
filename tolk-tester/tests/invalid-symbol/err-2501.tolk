const s1 = "asdf";

fun main() {
    var s1 redef = "d";
}

/**
@compilation_should_fail
@stderr `redef` for unknown variable
 */
