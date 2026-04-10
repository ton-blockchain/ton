fun main(x: int) {
    if (x) {
        if (x > 10) {
            return 1; // throw 1;
        } else if (true) {
            return -1;
        } else {
            return 2; // throw 2;
        }
    } else {
        {{return 1;}
            x = 30;}
    }
    assert(false, 10);
}

/**
@testcase | 0 | 1 | -1
@stderr warning: unreachable code
@stderr assert(false, 10)
@stderr x = 30
 */
