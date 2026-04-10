type UserId = int
type OwnerId = int

fun main() {
    var a = 10 as UserId | slice;
    match (a) {
        UserId => {}
        slice => {}
    }
    __expect_type(a, "UserId | slice");

    match (a) {
        OwnerId => {}
        slice => {}
    }
    return a;
}

/**
@compilation_should_fail
@stderr wrong pattern matching: `OwnerId` is not a variant of `UserId | slice`
@stderr OwnerId => {}
 */
