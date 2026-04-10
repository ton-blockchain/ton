fun main(cs: slice) {
    var cb = cs.tupleSize;
}

/**
@compilation_should_fail
@stderr field `tupleSize` doesn't exist in type `slice`
*/
