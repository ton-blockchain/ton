type UserId = int
type OwnerId = int

fun directAliasCheck(x: UserId?): bool {
    return x is OwnerId;
}

@inline
fun waitUntilNotOwner(x: UserId?): int {
    while (!(x is OwnerId)) {
    }
}

fun main(x: UserId?): bool {
    return directAliasCheck(x);
}

@method_id(102)
fun compileInlinedAlwaysTrueLoop(x: UserId?): int {
    return waitUntilNotOwner(x);
}

/**
@testcase | 0 | null | 0
@testcase | 0 | 123  | 0

@stderr warning: variable `x` of type `UserId?` can never be `OwnerId`, this condition is always false
@stderr warning: condition of `while` is always true

@fif_codegen
"""
  compileInlinedAlwaysTrueLoop() PROC:<{
    DROP
    AGAINEND
  }>
"""
 */
