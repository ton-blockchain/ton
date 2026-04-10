fun increment(mutate x: int) {
    x += 1;
}

fun cantModifyValDeclaredInMatch() {
    match (var a = (0 + 10 + 90) as int | slice) {
        int => increment(mutate a),
        slice => {},
    };
    match (val b = (0 + 10 + 90) as int | slice) {
        int => increment(mutate b),
        slice => {},
    };
}

/**
@compilation_should_fail
@stderr modifying immutable variable `b`
@stderr in function `cantModifyValDeclaredInMatch`
@stderr increment(mutate b)
 */
