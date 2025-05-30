fun demo(x: int) {

}

fun main() {
    10.demo();
}

/**
@compilation_should_fail
@stderr method `demo` not found, but there is a global function named `demo`
@stderr (a function should be called `foo(arg)`, not `arg.foo()`)
 */
