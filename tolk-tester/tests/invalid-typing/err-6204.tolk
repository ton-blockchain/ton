fun cantUnifyCoinsAndUInt8(n: int8, c: coins) {
    __expect_type(random() ? n : c as int8, "int8");
    __expect_type(random() ? n : c as int16, "int");
    random() ? n : c;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `int8` and `coins`
 */
