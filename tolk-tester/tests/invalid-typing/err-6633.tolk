fun main(c: cell) {
    var sender_address: slice = c.beginParse().loadAddress();
}

/**
@compilation_should_fail
@stderr can not assign `address` to variable of type `slice`
@stderr hint: unlike FunC, Tolk has a special type `address` (which is slice at the TVM level);
@stderr most likely, you just need `address` everywhere
@stderr hint: alternatively, use `as` operator for UNSAFE casting: `<some_expr> as slice`
 */
