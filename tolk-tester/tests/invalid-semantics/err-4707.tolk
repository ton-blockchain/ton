fun main(i: int): int {
    var f: (int) -> int = `__InMessage.getInMsgParam`;
    return f(i);
}

/**
@compilation_should_fail
@stderr internal compiler functions are not allowed to be called
 */
