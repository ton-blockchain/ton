fun main() {
    var n = (1, 2);
    (n, _, (_, n.0)) = (n, 0, (0, n.1));
}

/**
@compilation_should_fail
@stderr can not borrow `n.0` for mutation, because `n` is already being mutated by `assignment operator`
 */
