fun main() {
    var x = array<array<int>> [[""]];
    __expect_type(x, "array<array<int>>");
    return x;
}

/**
@compilation_should_fail
@stderr invalid `[...]` constructor: can not convert `string` to `int`
@stderr_avoid __expect_type
 */
