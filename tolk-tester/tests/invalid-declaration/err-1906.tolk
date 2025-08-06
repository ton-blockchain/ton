
fun onBouncedMessage(in: InMessageBounced) {
    return in.valueExtra;
}

/**
@compilation_should_fail
@stderr `onBouncedMessage` should return `void`
 */
