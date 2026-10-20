type TaggedInt = int

fun choose(flag: bool, tagged: TaggedInt) {
    return flag ? tagged : 0;
}

/**
@compilation_should_fail
@stderr types of ternary branches are incompatible: `TaggedInt` and `int`
 */
