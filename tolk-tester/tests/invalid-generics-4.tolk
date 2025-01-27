fun f<T>(x: T): void asm "NOP";

fun failInstantiatingAsmFunctionWithNon1Slot() {
    f((1, 2));
}

/**
@compilation_should_fail
@stderr can not call `f<T>` with T=(int, int), because it occupies 2 stack slots in TVM, not 1
 */
