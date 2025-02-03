fun main() {
    var ab = (1, 2);
    (ab, ab.1) = ((2, 3), 4);
}

/**
@compilation_should_fail
@stderr one variable both modified and read inside the same expression
*/
