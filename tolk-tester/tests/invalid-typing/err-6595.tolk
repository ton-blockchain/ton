fun main() {
    blockchain.configParam(true);
}

/**
@compilation_should_fail
@stderr can not pass `bool` to `int`
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as int`
@stderr caution! in TVM, bool TRUE is -1, not 1
 */
