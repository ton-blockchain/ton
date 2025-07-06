
fun main() {
    var z = 5;
    ++z;
    return z;
}

/**
@compilation_should_fail
@stderr Tolk has no increment operator
@stderr hint: use `i += 1`, not `i++`
 */
