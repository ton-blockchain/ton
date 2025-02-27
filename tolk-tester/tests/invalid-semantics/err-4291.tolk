struct T1<T1> { field: T1; }
struct CollisionName<T1> { item: T1<T1>; }

/**
@compilation_should_fail
@stderr type `T1` is not generic
 */
