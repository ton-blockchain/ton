fun cantCallMutatingMethod(c: cell) {
    val s: slice = c.beginParse();
    if (1) {
        var s: slice = c.beginParse();
        s.loadRef();       // this is ok, 's' is another variable
    }
    val i = s.loadUint(32);
}

/**
@compilation_should_fail
@stderr modifying immutable variable `s`
@stderr s.loadUint
 */
