struct A {
    f1: void;
}

fun main() {
    var a: A = {};      // f1 is void, can be omitted
    a.f1 = 100;
}

/**
@compilation_should_fail
@stderr can not assign `int` to field of type `void`
@stderr a.f1 = 100
 */
