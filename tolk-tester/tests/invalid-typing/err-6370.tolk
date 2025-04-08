fun getTensor(): (int?, int?) { return (5, null); }

fun testSmartCastsForFieldsDropAfterAssign() {
    var t = getTensor();
    if (t.0 != null && t.1 != null) {
        t = getTensor();
        return t.0 + t.1;
    }
    return -1;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int?`
 */
