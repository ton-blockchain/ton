fun main(any: any_address, usual: address) {
    any = usual;
}

/**
@compilation_should_fail
@stderr can not assign `address` to variable of type `any_address`
@stderr hint: use `as` operator for conversion: `<some_expr> as any_address`
 */
