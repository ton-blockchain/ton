fun T.copy(self) {
    return self;
}

fun main() {
    int.copy();
}

/**
@compilation_should_fail
@stderr too few arguments in call to `int.copy`, expected 1, have 0
@stderr int.copy();
 */
