fun main() {
    var x = array<array<int>> [[""]];
    __expect_type(x, "array<array<int>>");
    return x;
}

/**
@compilation_should_fail
@stderr type `array<slice>` is not assignable to `array<int>`
 */
