fun onInternalMessage() {
    onExternalMessage("");
}

fun onExternalMessage(body: slice) {
}

/**
@compilation_should_fail
@stderr onExternalMessage is a special entrypoint, it can not be called as a regular function
 */
