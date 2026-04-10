type MInt = int;

fun main() {
    return (5 as int8?) as MInt | slice | null;
}

/**
@compilation_should_fail
@stderr type `int8?` can not be cast to `MInt | slice | null`
 */
