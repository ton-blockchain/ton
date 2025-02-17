fun main() {
    var t: tuple = createEmptyTuple();
    t.asdf = 10;
}

/**
@compilation_should_fail
@stderr field `asdf` doesn't exist in type `tuple`
@stderr in function `main`
*/
