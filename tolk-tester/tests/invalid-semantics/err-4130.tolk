struct Point {
    x: int
    y: int
}

struct ROLine {
    readonly p1: Point
    readonly p2: Point
}

fun eq<T>(v: T) { return v }

fun f(l: ROLine) {
    l.p1; l.p2; l.p1.x; eq(l.p2.y); eq(eq(l).p2).y;

    l.p2!.x += 1;
}

/**
@compilation_should_fail
@stderr modifying readonly field `ROLine.p2` 
 */
