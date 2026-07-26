fun makeIt(): null | void { return null; }

fun main() {
    return makeIt().toCell().beginParse().remainingBitsCount();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `void?`
@stderr because `void | null` has no binary representation
 */
