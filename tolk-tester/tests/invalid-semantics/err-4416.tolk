fun f<T>(x: T): void asm "NOP";

fun failUsingGenericFunctionPartially() {
    var cb = f;
}

/**
@compilation_should_fail
@stderr can not use a generic function `f<T>` as non-call
 */
