fun int.f(self, v: (int, int) = (0, 1+"")) {
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int` and `slice`
 */
