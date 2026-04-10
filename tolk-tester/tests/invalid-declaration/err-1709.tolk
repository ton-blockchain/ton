global g: map<coins, slice>

/**
@compilation_should_fail
@stderr invalid `map`: type `coins` can not be used as a key
@stderr because its binary size is not constant: it's 4..124 bits
@stderr global g
 */
