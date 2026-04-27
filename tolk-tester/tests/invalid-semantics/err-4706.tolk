fun int.identity(self): self {
    return self;
}

fun main() {
    var cb = int.identity;
    return cb(10);
}

/**
@compilation_should_fail
@stderr saving `int.identity` into a variable is impossible, since it returns `self` and should be used as a method
 */
