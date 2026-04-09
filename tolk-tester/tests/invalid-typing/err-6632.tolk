fun (int, int).make(mutate self) {
    self.0 = 10;
    self.1 = 20;
}

fun demo(a: (int, int)?) {
    a!.make();
}

/**
@compilation_should_fail
@stderr can not mutate a temporary expression
 */
