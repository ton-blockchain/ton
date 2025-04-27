type MInt = int;
type MInt2 = MInt | int;
type MSlice = slice;
type MCell = cell;
type MBuilder = builder;

fun cantMatchAgainstNotVariant(a: int | slice | MBuilder) {
    match (a) {
        MInt2 => 10,
        MSlice => 20,
        MCell => 30,
    };
}

/**
@compilation_should_fail
@stderr wrong pattern matching: `cell` is not a variant of `int | slice | MBuilder`
@stderr MCell
 */
