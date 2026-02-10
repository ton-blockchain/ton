fun main() {
    var t: [int, int, int] = [0].get(0) ? [1,2] : [3,4];
}

/**
@compilation_should_fail
@stderr can not assign `[int, int]` to variable of type `[int, int, int]`
 */
