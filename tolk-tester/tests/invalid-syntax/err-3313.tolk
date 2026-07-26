fun main() {
    return 1 + /// asdfasdfsadf
           2
}

/**
@compilation_should_fail
@stderr doc comments '///' are allowed only at the top-level; use '//' comments here
 */
