type UserId = int32
type OwnerId = int32

fun main(v: UserId | slice) {
    match (v) {
        OwnerId => {}
        slice => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: use `UserId` instead of `OwnerId`
 */
