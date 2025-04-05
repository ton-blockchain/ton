type MNull = null;
type MNull2 = MNull;

fun main(t: MNull2) {
    t as null;
    t as MNull;
    t as MNull2;
    t as int?;
    t as int | slice | null;
    t as (int, int) | slice | MNull;

    t as int;           // error
}

/**
@compilation_should_fail
@stderr type `null` can not be cast to `int`
@stderr t as int
 */
