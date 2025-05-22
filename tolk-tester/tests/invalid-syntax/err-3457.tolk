type asdf = int;
const gcon = 10;

fun main() {
    match (10) {
        asdf => 1,      // it's match by type
    };

    var asdf = 5;
    match (10) {
        asdf => 2,      // also match by type
    };

    match (10) {
        gcon => 2,      // match by expression (it's constant)
    };

    var ten = 10;
    match (10) {
        ten => 10,      // not a constant, tries to lookup a type, fails
    }
}

/**
@compilation_should_fail
@stderr unknown type name `ten`
@stderr ten => 10
 */
