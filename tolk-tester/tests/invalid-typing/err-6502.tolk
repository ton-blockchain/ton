fun cantAutoInferUnionVariant() {
    var a: (int?, int?) | (int, int?) | int = 5;
    a = (1, 2) as (int?, int?);      // ok
    a = (1 as int?, 2 as int?);      // ok
    a = (1, 2);
}

/**
@compilation_should_fail
@stderr can not assign `(int, int)` to variable of type `(int?, int?) | (int, int?) | int`
 */
