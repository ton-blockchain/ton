fun main() {
    val s = "";
    val arr: array<(string, string, string, string)> = [(s, s, s, s)];
    return arr.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `array<(string, string, string, string)>`
@stderr because `(string, string, string, string)` is too big and won't fit into a nested cell
 */
