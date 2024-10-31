@pure
fun validate_input(input: cell): (int, int) {
    var (x, y, z, correct) = compute_data_size?(input, 10);
    assert(correct) throw 102;
}

@pure
fun someF(): int {
    var c: cell = begin_cell().end_cell();
    validate_input(c);
    return 0;
}

fun main() {}

/**
@compilation_should_fail
@stderr
"""
an impure operation in a pure function
assert(correct)
"""
*/
