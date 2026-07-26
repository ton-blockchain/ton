type Strange<T> = int

fun main() {
    match (null as int?) {
        Strange => {}
        null => {}
    }
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Strange<T>`, provide them manually
 */
