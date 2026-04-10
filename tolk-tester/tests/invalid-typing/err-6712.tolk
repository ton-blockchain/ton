fun matchDoesntCoverAllCases(a: int | slice?) {
    match (a) {
        int => {}
        slice => {}
    };
}

/**
@compilation_should_fail
@stderr `match` does not cover all possible types; missing types are: `null`
@stderr match (a)
 */
