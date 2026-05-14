fun main(a: array<int> | int) {
    return a is array;
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `array<T>`, provide them manually
 */
