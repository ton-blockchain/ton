fun acceptMutateNullableTensor(mutate self: (int, int)?) {
}

fun cantModifyTupleIndexWithTypeTransition() {
    var t = [1, null];
    t.1.acceptMutateNullableTensor();
}

/**
@compilation_should_fail
@stderr can not call method for mutate `(int, int)?` with object of type `null`
@stderr because mutation is not type compatible
 */
