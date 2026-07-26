fun test() {
    var x: int? = 1;
    var y: int? = 1;
    var z: int? = 1;
    var n = 3;
    while (n > 0 && x != null) {
        x = y;
        y = z;
        z = null;
        n -= 1;
        x + 1;
    }
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int`
@stderr x + 1
 */
