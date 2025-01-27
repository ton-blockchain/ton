fun failBitwiseNotOnBool() {
    var eq = 1 == 0;
    if (~eq) {
        return 0;
    }
}

/**
@compilation_should_fail
@stderr can not apply operator `~` to `bool`
 */
