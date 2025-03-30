fun invalidProvidingGenericTsToNotGeneric() {
    beginCell<builder>();
}

/**
@compilation_should_fail
@stderr calling a not generic function with generic T
 */
