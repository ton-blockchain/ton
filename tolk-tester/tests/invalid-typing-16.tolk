import "@stdlib/tvm-dicts.tolk"

fun testCantCallDictMethodsOnNullable(c: cell) {
    c.beginParse().loadDict().iDictDelete(16, 1);
}

/**
@compilation_should_fail
@stderr can not call method for `cell` with object of type `cell?`
 */
