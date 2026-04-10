
fun onInternalMessage(in: InMessage) {
    in.valueCoins = 123;
    return in.valueCoins;
}

/**
@compilation_should_fail
@stderr modifying an immutable variable
@stderr hint: fields of InMessage can be used for reading only
 */
