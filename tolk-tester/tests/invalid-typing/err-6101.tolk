type UserId = int

fun UserId.isPositive(self): bool {
    return self > 0
}

fun main() {
    var i: int = 1;
    return i.isPositive();
}

/**
@compilation_should_fail
@stderr method `isPositive` not found for type `int`
 */
