@pure
fun validate_input(input: cell): (int, int) {
    var (x, y, z, correct) = input.calculateSize(10);
    assert(correct) throw 102;
    return (x, y);
}

@pure
fun someF(): int {
    var c: cell = beginCell().endCell();
    validate_input(c);
    return 0;
}

fun main() {}

/**
@compilation_should_fail
@stderr an impure operation in a pure function
@stderr in function `validate_input`
@stderr assert(correct)
*/
