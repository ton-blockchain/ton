type MNull = null;
type IntOr16 = int | int16;

fun main(t: int?) {
    t as int? | null;                                // ok
    t as int? | slice;                               // ok
    t as (int, int) | MNull | int8 | IntOr16;        // ok

    t as slice?;
}

/**
@compilation_should_fail
@stderr type `int?` can not be cast to `slice?`
@stderr t as slice?
@stderr_avoid ok
 */
