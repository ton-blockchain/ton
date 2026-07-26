type UserId = int32
type OwnerId = int32

fun main(c: bool, u: UserId, o: OwnerId): UserId | builder | slice {
    return c ? (u as UserId | builder) : (o as OwnerId | slice);
}

/**
@compilation_should_fail
@stderr can not convert type `OwnerId | slice` to ternary result type `UserId | builder | slice`
 */
