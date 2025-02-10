fun main(cs: slice) {
    var cb = cs.tupleSize;
}

/**
@compilation_should_fail
@stderr referencing a method for `tuple` with object of type `slice`
*/
