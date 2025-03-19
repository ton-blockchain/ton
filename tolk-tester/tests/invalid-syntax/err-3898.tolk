fun main(x: int) {
    while (x > 0) {
        if (x == 10) {
            break;
        }
        x = x -1;
    }
}

/**
@compilation_should_fail
@stderr break/continue from loops are not supported yet
 */
