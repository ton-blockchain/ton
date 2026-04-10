
struct Options {
    o1: bool;
    o2: bool;
}

fun getBool() { return true; }

fun f(x: Options = {
    o1: true,
    o2: getBool(),
}) {
}

/**
@compilation_should_fail
@stderr not a constant expression
@stderr o2: getBool()
 */
