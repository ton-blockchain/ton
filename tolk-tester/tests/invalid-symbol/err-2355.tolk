
fun main() {
    match (10) {
        asdf => 1,
    };
}

/**
@compilation_should_fail
@stderr unknown type name `asdf`
@stderr asdf => 1
 */
