type MNull = null;
type MNull2 = MNull;

fun main(t: MNull2) {
    t as null;                               // ok
    t as MNull;                              // ok
    t as MNull2;                             // ok
    t as int?;                               // ok
    t as int | slice | null;                 // ok
    t as (int, int) | slice | MNull;         // ok

    t as int;           // error
}

/**
@compilation_should_fail
@stderr type `null` can not be cast to `int`
@stderr t as int
@stderr_avoid ok
 */
