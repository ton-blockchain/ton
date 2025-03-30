fun failAssignPlainNullToVariable() {
    var x = null;
}

/**
@compilation_should_fail
@stderr can not infer type of `x`, it's always null
@stderr specify its type with `x: <type>` or use `null as <type>`
 */
