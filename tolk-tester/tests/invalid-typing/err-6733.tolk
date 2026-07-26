fun Cell<T>.bad(self): T
    asm "NOP"

fun main(p: Cell<(int, int, slice?)>) {
    p.bad();
}

/**
@compilation_should_fail
@stderr can not call `Cell<T>.bad` with T=(int, int, slice?), because it occupies 3 stack slots in TVM, not 1
 */
