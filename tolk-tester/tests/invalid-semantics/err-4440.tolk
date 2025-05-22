struct NumberFields {
    `-1`: int;
    `0`: int;
    `100500`: int;
}

fun main() {
    var b: NumberFields = { `-1`: -1, `0`: 0, `100500`: 100500 };
    b.0;                // ok
    b.`0`;              // ok
    b.`-1`;             // ok
    b.`100500`;         // ok
    b.100500;           // ok
    b.123;
}

/**
@compilation_should_fail
@stderr field `123` doesn't exist in type `NumberFields`
@stderr b.123
 */
