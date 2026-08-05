fun main(): void {
}

@method_id(0)
fun alsoZero(): int {
    return 1;
}

/**
@compilation_should_fail
@stderr method_id collision: `main` and `alsoZero` produce the same method_id=0
@stderr fun alsoZero
*/
