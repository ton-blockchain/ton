type MInt = int;

fun f1(v: int): int { return v; }

fun main(t: int?) {
    f1 as (MInt) -> MInt;
    f1 as (int8) -> uint16;

    f1 as ((int | slice) -> int);
}

/**
@compilation_should_fail
@stderr type `(int) -> int` can not be cast to `(int | slice) -> int`
 */
