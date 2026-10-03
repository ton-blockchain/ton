fun cantReturnVoidAndNonVoid(a: int | slice | builder) {
    throw 123;
    match (a) {
        int => { return; }
        slice => {}
        builder => ""
    };
    return 123;
}

/**
@compilation_should_fail
@stderr mixing void and non-void returns
@stderr int => { return; }
@stderr note: this return has a value
@stderr return 123;
 */
