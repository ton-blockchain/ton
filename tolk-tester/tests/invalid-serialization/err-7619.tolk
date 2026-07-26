fun main() {
    var arr: array<()> = [];
    arr.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `array<()>`
@stderr because `()` has zero binary size, and an array can't be deserialized back
 */
