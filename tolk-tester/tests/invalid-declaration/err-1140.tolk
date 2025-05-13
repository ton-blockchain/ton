fun okWithJustInt(a: int8, b: int16): int {
    if (a > 10) {
        return a;
    }
    return b;
}

fun okWithManualUnion(a: int8, b: int16): int16 | int8 {
    if (a > 10) {
        return a;
    }
    return b;
}

fun autoInferUnionType(a: int8, b: int16) {
    if (a > 10) {
        return a;
    }
    return b;
}

/**
@compilation_should_fail
@stderr function `autoInferUnionType` calculated return type is `int8 | int16`; probably, it's not what you expected
@stderr declare `fun (...): <return_type>` manually
@stderr fun autoInferUnionType
 */
