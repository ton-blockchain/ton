@pure
fun myDictDeleteStrict(mutate self: cell, keyLen: int, key: int): bool
    asm(key self keyLen) "DICTIDEL";


fun testCantCallDictMethodsOnNullable(c: cell) {
    c.beginParse().loadDict().myDictDeleteStrict(16, 1);
}

/**
@compilation_should_fail
@stderr can not call method for `cell` with object of type `dict`
 */
