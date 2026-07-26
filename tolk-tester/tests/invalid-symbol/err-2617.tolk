fun (int | (T, slice)).m(self) {
}

fun main() {
    1.m();
}

/**
@compilation_should_fail
@stderr method `m` not found for type `int`
@stderr (but it exists for type `int | (T, slice)`)
 */
