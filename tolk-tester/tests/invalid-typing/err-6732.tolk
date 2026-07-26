struct Point {
    x: int32
    y: int32
}

fun Cell<T>.accept(self, value: T): void
    asm "2DROP" "DROP"

fun main() {
    val c = Point { x: 1, y: 2 }.toCell();
    c.accept(Point { x: 3, y: 4 });
}

/**
@compilation_should_fail
@stderr can not call `Cell<T>.accept` with T=Point, because it occupies 2 stack slots in TVM, not 1
 */
