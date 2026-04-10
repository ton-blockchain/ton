type MInt = int;

fun main() {
    var value = MInt;
}

/**
@compilation_should_fail
@stderr `MInt` only refers to a type, but is being used as a value here
*/
