fun increment(mutate x: int) {
    x += 1;
}

fun main() {
    var i: int;
    if (true) {
        i = 10;
    }
    increment(mutate i);        // ok

    var j: int8 | int16;
    if (random.uint256()) {
        (j, _) = (10 as int8, i);
    } else {
        return;
    }
    match (j) {     // ok
        int8 => increment(mutate i),
        int16 => increment(mutate j),
    }

    var k: int;
    {
        increment(mutate k);
    }
}

/**
@compilation_should_fail
@stderr using variable `k` before it's definitely assigned
 */
