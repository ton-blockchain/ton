fun main(any: any_address, usual: address) {
    usual = any;
}

/**
@compilation_should_fail
@stderr can not assign `any_address` to variable of type `address`
@stderr hint: use `any_addr.castToInternal()` to check and get `address`
@stderr hint: alternatively, use `as` operator for UNSAFE casting: `<some_expr> as address`
 */
