struct Point { x: int; y: int; }

fun Point?.methodOverNullable(self) {}

fun cantAssignNullableObjectField(a: Point?) {
    if (a != null) {
        a.x = 10;           // ok
    }
    a.methodOverNullable(); // ok
    a.y = 20;
}

/**
@compilation_should_fail
@stderr can not access field `y` of a possibly nullable object `Point?`
@stderr check it via `obj != null` or use non-null assertion `obj!` operator
 */
