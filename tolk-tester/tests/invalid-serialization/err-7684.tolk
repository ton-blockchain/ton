struct St {
}

fun getStorage() {
    return lazy St.fromCell(contract.getData());
}

/**
@compilation_should_fail
@stderr error: incorrect `lazy` operator usage, it's not directly assigned to a variable
 */
