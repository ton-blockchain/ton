type MInt = int
struct WI { v: MInt }

struct P {
    w: map<address, (bool, WI)>
}

fun main() {

}

/**
@compilation_should_fail
@stderr invalid `map`: type `(bool, WI)` can not be used as a value
@stderr because it can not be serialized
@stderr because element `tensor.1` of type `WI` can't be serialized
@stderr because field `WI.v` of type `MInt` can't be serialized
@stderr because alias `MInt` expands to `int`
@stderr because type `int` is not serializable, it doesn't define binary width
@stderr hint: replace `int` with `int32` / `uint64` / `coins` / etc.
@stderr w: map<address, (bool, WI)>
 */
