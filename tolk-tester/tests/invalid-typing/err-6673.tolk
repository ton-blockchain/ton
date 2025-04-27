fun cantAutoInferUnionVariant() {
    var a: int8 | int16 = 0 as int8; // ok
    a = 0 as int16;                  // ok
    if (a is int8) {
        a = 1 as int8;               // ok
    }
    a = 2;
}

/**
@compilation_should_fail
@stderr can not assign `int` to variable of type `int8 | int16`
@stderr a = 2;
 */
