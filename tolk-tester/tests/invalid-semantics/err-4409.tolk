fun main(cond: bool) {
    // `unknown` is not a valid hint — otherwise, what should be stored?
    var cc: unknown = cond ? (1,2) : (1,2,3);
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `(int, int)` and `(int, int, int)`
 */
