type Pair2 = (int, int);
type Pair2Or3 = Pair2 | (int, int, int);

fun matchDoesntCoverAllCases(a: Pair2Or3 | slice | [[int]] | builder) {
    if (a !is slice) {
        match (a) {
            (int, int) => {}
            [[int]] => {}
        }
    }
}

/**
@compilation_should_fail
@stderr `match` does not cover all possible types; missing types are: `(int, int, int)`, `builder`
 */
