struct Wrapper<T> { item: T; }
fun Wrapper<T?>.createNull(): Wrapper<T?> { return { item: null } }

fun main() {
    Wrapper<int>.createNull();
}

/**
@compilation_should_fail
@stderr method `createNull` not found for type `Wrapper<int>`
@stderr (but it exists for type `Wrapper<T?>`)
 */
