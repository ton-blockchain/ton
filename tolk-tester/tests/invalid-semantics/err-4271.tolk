fun main() {
    fun(x: int, y: () -> void = fun() {}) {  }
}

/**
@compilation_should_fail
@stderr not a constant expression
 */
