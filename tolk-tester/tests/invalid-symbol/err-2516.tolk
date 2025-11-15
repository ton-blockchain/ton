fun main() {
    fun(x: int, y: asdf) {  }
}

/**
@compilation_should_fail
@stderr unknown type name `asdf`
 */
