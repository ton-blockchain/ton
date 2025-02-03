fun main() {
    var c = 1;
    (c, c) = (2, 3);
}

/**
@compilation_should_fail
@stderr one variable modified twice inside the same expression
*/
