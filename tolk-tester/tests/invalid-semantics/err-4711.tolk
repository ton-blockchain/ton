struct Pair<T1, T2> {
    first: T1;
    second: T2;
}

fun main() {
    var ok1: Pair<int?, int?> = { first: null, second: null };
    var ok2: Pair<int?, int?> = Pair { first: null, second: null };
    var ok3 = Pair<int?, int?> { first: null, second: null };
    var ok4 = Pair { first: null as int?, second: null as (int, int)? };
    var ok5 = Pair { first: null as int?, second: null as slice? };   // ok since doesn't reach type checking
    var ok6 = Pair<null, null> { first: null, second: null };

    var err = Pair { first: null, second: null };
}

/**
@compilation_should_fail
@stderr can not deduce T1 for generic struct `Pair<T1, T2>`
@stderr instantiate it manually with `Pair<...>`
@stderr var err =
 */
