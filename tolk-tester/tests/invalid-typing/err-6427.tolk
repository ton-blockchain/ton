fun cantReturnVoidAndNonVoid(a: int | slice | builder) {
    match (a) {
        int => { return; }
        slice => {}
        builder => ""
    };
    return 123;
}

/**
@compilation_should_fail
@stderr mixing void and non-void returns in function
 */
