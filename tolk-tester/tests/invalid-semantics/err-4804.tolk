fun main() {
    var functions = (beginCell, beginCell);
    var b = functions.1();   // ok
    var c = functions.2();   // error
}

/**
@compilation_should_fail
@stderr invalid tensor index, expected 0..1
@stderr functions.2()
 */
