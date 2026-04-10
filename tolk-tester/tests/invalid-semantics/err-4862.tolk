struct Pair<T1, T2> {
    first: T1;
    second: T2;
}

fun cantProvideDuplicateField() {
    var p: Pair<int?, slice?> = {
        first: null,
        second: null,
        first: 3,
    };
}

/**
@compilation_should_fail
@stderr duplicate field initialization
@stderr first: 3
 */
