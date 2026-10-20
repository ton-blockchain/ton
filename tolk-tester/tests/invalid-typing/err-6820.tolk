fun cantUnifyBitsNAndBitsM(n: bits64, c: bits128) {
    __expect_type(random.uint256() ? n : c as bits64, "bits64");
    random.uint256() ? n : c;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `bits64` and `bits128`
@stderr random.uint256()
 */
