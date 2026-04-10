import "../imports/imports-strings"

fun t(): int {
    return "abc".calculateLength();
}

/**
@compilation_should_fail
@stderr Using a non-imported symbol `string.calculateLength`
@stderr hint: forgot to import "@stdlib/strings"?
 */
