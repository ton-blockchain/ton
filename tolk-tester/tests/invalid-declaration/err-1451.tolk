@method_id(42)
fun first(): int {
    return 1;
}

@method_id(42)
fun second(): int {
    return 2;
}

fun main(): void {
}

/**
@compilation_should_fail
@stderr method_id collision: `first` and `second` produce the same method_id=42
@stderr fun second
*/
