fun main(v: int32 | int64): array<unknown> {
    return [
        match (v) {
            int32 => v,
            int64 => v,
        }
    ]
}

/**
@compilation_should_fail
@stderr type of `match` was inferred as `int32 | int64`; probably, it's not what you expected
 */
