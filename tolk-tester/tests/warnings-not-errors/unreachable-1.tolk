fun main(x: int) {
    if (x) {
        x = 10;;;;;
        return x;;;
        x = 20;
    }
    return -1;
}

/**
@testcase | 0 | 1 | 10
@stderr warning: unreachable code
@stderr x = 20;
 */
