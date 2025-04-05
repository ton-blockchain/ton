fun getIntOrSlice() { return 5 as int | slice; }

fun autoInferUnionType(a: int | slice | null) {
    var cc = match (a) {
        int => a = getIntOrSlice(),
        slice => return beginCell(),
        null => return -20
    };
    return cc;
}

/**
@compilation_should_fail
@stderr function `autoInferUnionType` calculated return type is `builder | int | slice`; probably, it's not what you expected
@stderr fun autoInferUnionType
 */
