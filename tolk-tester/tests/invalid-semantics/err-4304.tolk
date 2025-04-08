type MInt = int;

fun main() {
    var value = MInt;
}

/**
@compilation_should_fail
@stderr type `MInt` can not be used as a value
*/
