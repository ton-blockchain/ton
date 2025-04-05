fun cantInferUnionTypeWithoutHint(a: int | slice | builder) {
    var ok1: builder | (slice | int | int) = match (a) {
        int => a,
        slice => a,
        builder => a,
    };
    var ok2 = match (a) {
        int => a,
        slice => a,
        builder => a,
    } as builder | (slice | int | int);
    var ok3 = match (a) {
        int => a     as builder | slice | int,
        slice => a   as builder | slice | int,
        builder => a as builder | slice,
    };

    var err = match (a) {
        int => a!,
        slice => a!,
        builder => a!,
    };
}

/**
@compilation_should_fail
@stderr type of `match` was inferred as `int | slice | builder`; probably, it's not what you expected
@stderr assign it to a variable `var v: <type> = match (...) { ... }` manually
@stderr err = match (a)
 */
