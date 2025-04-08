@method_id(123)
get fun hello(x: int, y: int): (int, int) {
    return (x, y);
}

/**
@compilation_should_fail
@stderr @method_id can be specified only for regular functions
*/
