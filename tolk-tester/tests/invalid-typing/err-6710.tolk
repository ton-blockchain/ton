fun f(s: slice) {

}

fun testCantPassAddressToSlice() {
    var a = createAddressNone();
    f(a as slice);      // ok
    f(a);
}

/**
@compilation_should_fail
@stderr can not pass `any_address` to `slice`
@stderr f(a);
 */
