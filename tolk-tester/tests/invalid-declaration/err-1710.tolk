struct WithRef {
    v: int32;
    r: cell?;
}

fun main(s: map<WithRef, int8>) {

}

/**
@compilation_should_fail
@stderr invalid `map`: type `WithRef` can not be used as a key
@stderr because it may contain a cell reference, not only data bits
 */
