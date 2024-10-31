fun main(x: int) {
    assert(x > 0);
}

/**
@compilation_should_fail
@stderr expected `throw excNo` after assert, got `;`
 */
