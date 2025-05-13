fun cantUnifyCoinsAndUInt8(n: int8, c: coins) {
    __expect_type(random.uint256() ? n : c as int8, "int8");
    __expect_type(random.uint256() ? n as int16 : c as int16, "int16");

    var withHint: int = random.uint256() ? n : c;  // ok
    var withoutHint   = random.uint256() ? n : c;  // error
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `int8` and `coins`
@stderr var withoutHint
 */
