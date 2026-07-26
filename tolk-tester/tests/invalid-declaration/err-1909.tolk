
fun onInternalMessage(in: InMessage) {
    var body = (fun() {
        return in.body;
    })();
    body.skipBits(0);
}

/**
@compilation_should_fail
@stderr capturing `InMessage` in a lambda is prohibited
 */
