fun f1(a: int) {}
fun f2(mutate a: int, b: int) {}
fun f3(mutate a: int, mutate b: int, c: int) {}
fun f4(a: int, b: int = 0, c: int = 0, d: int = 0) {}
fun f5<T3 = void, T4 = void>(a: int, b: slice, c: T3, d: T4) {}
fun f6<T3 = void, T4 = int>(a: int, b: slice, c: T3, d: T4) {}

fun main(x: int, s: slice) {
    f1();
    f2(x);
    f3(mutate s);
    f4();
    f5(5);
    f6(6);
    return (address(), string.crc32());
}

/**
@compilation_should_fail
@stderr too few arguments in call to `f1`, expected 1, have 0
@stderr too few arguments in call to `f2`, expected 2, have 1
@stderr function `f2` mutates parameter `a`
@stderr too few arguments in call to `f3`, expected 3, have 1
@stderr can not pass `slice` to mutate `int`, because mutation is not type compatible
@stderr too few arguments in call to `f4`, expected 1, have 0
@stderr too few arguments in call to `f5<void,void>`, expected 2, have 1
@stderr too few arguments in call to `f6<void,int>`, expected 4, have 1
@stderr too few arguments in call to `address`, expected 1, have 0
@stderr too few arguments in call to `string.crc32`, expected 1, have 0
 */
