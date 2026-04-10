fun main(x: int, y: int) {
    if (x = 1) {
        y = 10;
    }
    assert ((y = 20)) throw 123;
    return x + y;
}

/**
@testcase | 0 | 0 0 | 21
@stderr assignment inside condition, probably it's a misprint
@stderr if (x = 1)
@stderr assert ((y = 20)) 
 */
