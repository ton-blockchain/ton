struct A {
    f1: never;
}

fun main() {
    var a: A = {};      // f1 is never, can be omitted
    a.f1 = 100;
}

/**
@compilation_should_fail
@stderr can not assign `int` to field of type `never`
@stderr a.f1 = 100
 */
