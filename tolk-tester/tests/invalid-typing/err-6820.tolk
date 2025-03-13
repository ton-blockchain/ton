fun cantUnifyBytesNAndBytesM(n: bytes8, c: bytes16) {
    __expect_type(random() ? n : c as bytes8, "bytes8");
    random() ? n : c;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `bytes8` and `bytes16`
 */
