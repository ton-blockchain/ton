
fun wipe(mutate s: slice?) {
    s = null;
}

fun main(v: slice | int | null) {
    if (v is slice || v == null) {
        __expect_type(v, "slice?");
        // this is not supported, because of a different stack layout (no UTag after smart cast)
        wipe(mutate v);
    }
}

/**
@compilation_should_fail
@stderr can not pass `slice | int | null` to mutate `slice?`, because mutation is not type compatible
 */
