
fun int.demo(self) {

}

fun main() {
    (10 as int?).demo();
}

/**
@compilation_should_fail
@stderr method `demo` not found for type `int?`
@stderr (but it exists for type `int`)
 */
