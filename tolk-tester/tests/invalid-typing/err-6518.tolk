type MNull = null;
type IntOr16 = int | int16;

fun main(t: int?) {
    t as int? | null;
    t as int? | slice;
    t as (int, int) | MNull | int8 | IntOr16;

    t as slice?;
}

/**
@compilation_should_fail
@stderr type `int?` can not be cast to `slice?`
@stderr t as slice?
 */
