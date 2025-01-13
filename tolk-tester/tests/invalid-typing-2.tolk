fun main() {
    var tri: (int, int) = (10, false);
    return;
}

/**
@compilation_should_fail
@stderr can not assign `(int, bool)` to variable of type `(int, int)`
 */
