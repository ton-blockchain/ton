type UserId = int32
type OwnerId = int32

fun main(c: bool, u: UserId, o: OwnerId): UserId {
    return match (c) {
        true => u,
        false => o
    };
}

/**
@compilation_should_fail
@stderr can not convert type `OwnerId` to match result type `UserId`
 */
