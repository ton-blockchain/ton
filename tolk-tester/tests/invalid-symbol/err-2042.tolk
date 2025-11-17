fun main(outerParam: int) {
    fun(x: int) {
        x + outerParam
    }
}

/**
@compilation_should_fail
@stderr undefined symbol `outerParam`
 */
