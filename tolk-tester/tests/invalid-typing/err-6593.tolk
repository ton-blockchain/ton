type UserId = int
type OwnerId = int

fun f(v: UserId) {
    match (v) {
        OwnerId => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: `OwnerId` is not a variant of `UserId`
 */
