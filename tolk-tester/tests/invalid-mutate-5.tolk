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
@stderr modifying an immutable variable `s` (call a mutating method)
@stderr s.loadUint
 */
