fun main(v: bits16) {
    v as slice?;
}

/**
@compilation_should_fail
@stderr type `bits16` can not be cast to `slice?`
@stderr use an intermediate cast: `xxx as slice as slice?`
 */
