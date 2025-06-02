@deprecated("anything")
@custom("anything")
global a: int;

@deprecated
@custom
const ASDF = 1;

@custom("props", {allowed: false, ids: [1,2,3]})
@deprecated
fun f() {}

@custom({
    `type`: 123,
    value: 19
})
@custom("another", 12, "annotation")
struct TTT {}

@deprecated
type MyMsg = int;

@custom(1,2,3,4)
@custom({function: "main", contract: self})
fun main() {
    return 0;
}

/**
@testcase | 0 | | 0
 */
