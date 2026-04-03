fun rand() { return random.uint256() }

fun cantAutoInferUnionInTernary() {
    var dd: int | string = rand() ? 1 : "";       // ok
    var cc = (rand() ? 1 : "") as int|string;     // ok
    rand() ? 1 as int|string : "" as int|string;  // ok
    rand() ? 1 as int|string : "";                // ok
    rand() ? 1 : "" as int|string|builder;        // ok
    rand() ? 1 as int|string|null : "" as string?;// ok

    var sub = rand() ? (1, 2 as int|string) : (1, "");
    __expect_type(sub, "(int, int | string)");

    rand() ? 1 : "";
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `int` and `string`
 */
