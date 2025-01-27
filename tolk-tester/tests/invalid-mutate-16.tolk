fun cantCallMutatingFunctionWithAssignmentLValue() {
    var t: tuple = createEmptyTuple();
    (t = createEmptyTuple()).tuplePush(1);
}

/**
@compilation_should_fail
@stderr assignment can not be used as lvalue
 */
