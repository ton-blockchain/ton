fun rand() { return random.uint256() }

fun cantAutoInferUnionInTernary() {
    var dd: int | slice = rand() ? 1 : "";        // ok
    var cc = (rand() ? 1 : "") as int|slice;      // ok
    rand() ? 1 as int|slice : "" as int|slice;    // ok
    rand() ? 1 as int|slice : "";                 // ok
    rand() ? 1 : "" as int|slice|builder;         // ok
    rand() ? 1 as int|slice|null : "" as slice?;  // ok

    var sub = rand() ? (1, 2 as int|slice) : (1, "");
    __expect_type(sub, "(int, int | slice)");

    rand() ? 1 : "";
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `int` and `slice`
 */
