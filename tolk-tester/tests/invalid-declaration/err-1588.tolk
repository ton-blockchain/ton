struct A<T> {
    b: B<T>
}

struct B<T> {
    a: A<T>
}

type X = A<int>

fun main(): int {
    return 0;
}

/**
@compilation_should_fail
@stderr size is infinity due to recursive fields
 */
