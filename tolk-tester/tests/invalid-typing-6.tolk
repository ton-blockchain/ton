fun failWhenTernaryConditionNotInt(cs: slice) {
    return cs ? 1 : 0;
}

/**
@compilation_should_fail
@stderr condition of ternary operator must be an integer
 */
