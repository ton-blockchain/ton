
fun handle(in: InMessage) {
}

fun onInternalMessage(in: InMessage) {
    return handle(in);
}

/**
@compilation_should_fail
@stderr using `in` as an object is prohibited, because `InMessage` is a built-in struct, its fields are mapped to TVM instructions
 */
