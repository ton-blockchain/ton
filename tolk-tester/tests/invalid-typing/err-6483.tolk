fun recursive() {
    return fun() {
        return recursive;
    }
}

/**
@compilation_should_fail
@stderr could not infer return type of `recursive`, because it appears in a recursive call chain
@stderr declare `fun (...): <return_type>` manually 
 */
