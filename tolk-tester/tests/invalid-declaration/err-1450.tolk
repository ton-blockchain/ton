fun main(): void {
}

@method_id(104128)
fun manualCollision(): int {
    return 2;
}

get fun balance(): int {
    return 1;
}

/**
@compilation_should_fail
@stderr method_id collision: `manualCollision` and `balance` produce the same method_id=104128
@stderr get fun balance
*/
