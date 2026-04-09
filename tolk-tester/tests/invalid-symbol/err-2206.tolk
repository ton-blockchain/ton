import "../imports/imports-ext-method"

fun t(): int {
    return 5.methodForInt<string>("");
}

/**
@compilation_should_fail
@stderr Using a non-imported symbol `int.methodForInt`
@stderr hint: forgot to import "has-ext-method.tolk"?
 */
