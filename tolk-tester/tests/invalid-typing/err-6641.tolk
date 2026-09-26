type TaggedInt1 = int
type TaggedInt2 = int

fun TaggedInt1?.choose(self, b: TaggedInt2) {
    return self ?? b
}

/**
@compilation_should_fail
@stderr type of operator `??` is `TaggedInt1`; probably, it's not what you expected
 */
