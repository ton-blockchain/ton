struct T1<T1> { field: T1; }
struct CollisionName<T1> { item: T1<T1>; }

/**
@compilation_should_fail
@stderr unknown type name `T1`
@stderr item: T1<T1>
 */
