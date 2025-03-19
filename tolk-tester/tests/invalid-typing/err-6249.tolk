fun cantUnifyBytesNAndSlice(n: slice, c: bytes16) {
    __expect_type(random.uint256() ? n : c as slice, "slice");
    __expect_type(random.uint256() ? n : c as slice?, "slice?");
    random.uint256() ? n : c;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `slice` and `bytes16`
 */
