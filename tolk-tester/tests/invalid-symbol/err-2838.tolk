fun test(a: array<int>): int {
    return a[0];
}

/**
@compilation_should_fail
@stderr use `array.get(idx)`, not `array[idx]`
 */
