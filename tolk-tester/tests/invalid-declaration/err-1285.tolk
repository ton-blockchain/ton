struct Pair<T1, T2> {
    first: T1;
    second: T2;
}

struct DoesntWork {
    p: Pair<int>;
}

/**
@compilation_should_fail
@stderr struct `Pair<T1, T2>` expects 2 type arguments, but 1 provided
@stderr p: Pair<int>
 */
