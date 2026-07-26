struct Box<T> {
    value: T;
}

type Tagged<T> = Box<T>

fun makeTagged(): Tagged<uint8> | uint8 {
    val tagged: Tagged<uint8> = { value: 1 };
    return tagged;
}

fun main() {
    var x: Tagged<uint8> | uint8 = makeTagged();
    match (x) {
        Tagged => {}
        uint8 => {}
    }
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Tagged<T>`, provide them manually
@stderr Tagged => {}
 */
