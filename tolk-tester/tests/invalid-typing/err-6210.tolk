fun main() {
    beginCell().storeSlice(fun(a: slice) { return 123 })
}

/**
@compilation_should_fail
@stderr can not pass `(slice) -> int` to `slice` 
 */
