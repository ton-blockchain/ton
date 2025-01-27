fun failWhenTernaryConditionNotInt(cs: slice) {
    return cs ? 1 : 0;
}

/**
@compilation_should_fail
@stderr can not use `slice` as a boolean condition
 */
