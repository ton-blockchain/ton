fun cantUnifyBytesNAndBytesM(n: bytes8, c: bytes16) {
    __expect_type(random.uint256() ? n : c as bytes8, "bytes8");
    random.uint256() ? n : c;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `bytes8` and `bytes16`
 */
