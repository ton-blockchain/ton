struct Point {
    x: int8
    y: int8
}

fun Cell<T>.myMethod<U>(self, v: U): int 
    asm "DROP" "HASHCU"

fun main() {
    Point{x:10,y:20}.toCell().myMethod(4);      // ok
    Point{x:10,y:20}.toCell().myMethod((4, 5));
}

/**
@compilation_should_fail
@stderr can not call `Cell<T>.myMethod<U>` with U=(int, int), because it occupies 2 stack slots in TVM, not 1
 */
