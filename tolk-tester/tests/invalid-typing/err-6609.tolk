fun main() {
    var c: (int) -> void = fun(c) {
        c = ()
    }
}

/**
@compilation_should_fail
@stderr can not assign `()` to variable of type `int`
@stderr in lambda in function `main`
 */
