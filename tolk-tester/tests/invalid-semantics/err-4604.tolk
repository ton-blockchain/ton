fun invalidProvidingGenericTsToNotGeneric() {
    beginCell<builder>();
}

/**
@compilation_should_fail
@stderr type arguments not expected here
 */
