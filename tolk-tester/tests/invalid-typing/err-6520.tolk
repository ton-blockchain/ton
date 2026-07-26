type UserId = int
type OwnerId = int

fun main(x: UserId | OwnerId) {}

/**
@compilation_should_fail
@stderr union variants `UserId` and `OwnerId` have the same runtime representation
 */
