global gg: (int, [int, int]);

fun main() {
    (gg.1.0, gg.1, gg.1.1) = (0, [1, 2], 3);
}

/**
@compilation_should_fail
@stderr one variable both modified and read inside the same expression
*/
