@deprecated("anything")
@custom("anything")
@custom.a.b({})
global a: int;

@deprecated
@custom
const ASDF = 1;

@custom("props", {allowed: false, ids: [1,2,3]})
@deprecated
fun f() {}

@custom({
    `type sp`: 123,
    value: 19
})
@custom("another", 12, "annotation")
@custom.dot
struct TTT {}

@deprecated
type MyMsg = int;

@custom(1,2,3,4)
@custom({function: "main", contract: self})
@test.first
@test.anything("asdf")
fun main() {
    return 0;
}

/**
@testcase | 0 | | 0
 */
