fun main() {
    (1 as int16) as (int | int16);     // ok, exact match
    (1 as int8)  as (int | int32);     // ok, int accepts int8, but int32 not

    1 as (int16 | int32);              // error, ambiguous
}

/**
@compilation_should_fail
@stderr type `int` can not be cast to `int16 | int32`
 */
