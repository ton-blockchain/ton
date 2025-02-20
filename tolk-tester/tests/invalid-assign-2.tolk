fun incThree(mutate a: int, mutate b: int, mutate c: int) {}

fun main() {
    var c = [[[1, 2]]];
    incThree(mutate c.0.0.0, mutate c.0.0.1, mutate c.0.0.0);
}

/**
@compilation_should_fail
@stderr one variable modified twice inside the same expression
*/
