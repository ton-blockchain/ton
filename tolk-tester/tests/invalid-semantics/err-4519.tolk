struct value { id: int; }

fun main() {
    var value = 100;
    var value2 = 200;
    var obj1 = value { id: 100 };       // "value" refs to a struct, it's parsed as a type
    var obj2 = value2 { id: 200 };      // no struct "value2"
}

/**
@compilation_should_fail
@stderr unknown type name `value2`
@stderr obj2 = value2
 */
