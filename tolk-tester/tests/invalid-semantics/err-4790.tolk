fun onInternalMessage() {
    var f = onExternalMessage;
}

fun onExternalMessage(body: slice) {
}

/**
@compilation_should_fail
@stderr can not get reference to this function, it's a special entrypoint
 */
