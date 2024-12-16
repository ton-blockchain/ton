global set: int;

@pure
fun someF(): int {
    var set redef = 0;
    return set;
}

/**
@compilation_should_fail
@stderr
"""
an impure operation in a pure function
var set
"""
*/
