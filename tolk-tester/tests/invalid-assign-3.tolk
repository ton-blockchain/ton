global gg: (int, int);

fun main() {
    [gg.0, gg.1, gg.0] = [0, 1, 0];
}

/**
@compilation_should_fail
@stderr one variable modified twice inside the same expression
*/
