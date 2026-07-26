fun myIsEmpty<T>(m: map<int32, T>) {
    return m.isEmpty()
}

fun main() {
    return myIsEmpty([])
}

/**
@compilation_should_fail
@stderr can not deduce T for generic function `myIsEmpty<T>`
 */
