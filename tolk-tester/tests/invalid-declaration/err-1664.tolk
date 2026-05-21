@method_id(262144)
fun exported(): int {
    return 1;
}

/**
@compilation_should_fail
@stderr invalid @method_id
 */
