type asdf = int;

fun main() {
    match (10) {
        asdf => 1,      // it's match by type
    };

    var asdf = 5;
    match (10) {
        asdf => 2,      // it's match by expression now
    };
}

/**
@compilation_should_fail
@stderr `match` by expression is not supported yet
@stderr asdf => 2
 */
