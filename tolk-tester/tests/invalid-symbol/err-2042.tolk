fun main(outerParam: int) {
    fun(x: int) {
        x + outerParam + undefSymbol
    }
}

/**
@compilation_should_fail
@stderr undefined symbol `undefSymbol`
 */
