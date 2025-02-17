struct value { id: int; }

fun main() {
    var value = 100;
    var obj = value { id: 100 };
}

/**
@compilation_should_fail
@stderr `value` does not name a struct
@stderr obj = value
 */
