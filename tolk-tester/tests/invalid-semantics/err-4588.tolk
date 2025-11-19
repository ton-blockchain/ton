struct Point {
    x: int;
    y: int;
}

fun getX(i: int) { return i; }
fun Point.getX(self) { return self.x; }

fun main() {
    var Point = 10;
    Point.getX();
}

/**
@compilation_should_fail
@stderr method `getX` not found for type `int`
 */
