fun cantAutoInferUnionInTernary() {
    var dd: int | slice = random() ? 1 : "";        // ok
    var cc = (random() ? 1 : "") as int|slice;      // ok
    random() ? 1 as int|slice : "" as int|slice;    // ok
    random() ? 1 as int|slice : "";                 // ok
    random() ? 1 : "" as int|slice|builder;         // ok
    random() ? 1 as int|slice|null : "" as slice?;  // ok

    var sub = random() ? (1, 2 as int|slice) : (1, "");
    __expect_type(sub, "(int, int | slice)");

    random() ? 1 : "";
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `int` and `slice`
 */
