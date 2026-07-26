struct Loop {
    x: Loop?
}

/**
@compilation_should_fail
@stderr struct `Loop` size is infinity due to recursive fields
 */
