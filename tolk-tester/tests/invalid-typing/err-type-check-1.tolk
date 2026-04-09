// @compilation_should_fail
// various tests for type checker errors (not all! there are lots of independent err-xxxx)

enum Color { Red, Green, Blue }

struct Point { a: int; b: int; }

fun Point.assign(mutate self) { }

// @stderr can not apply operator `==` to `Color` and `int`
// @stderr can not apply operator `+` to `Color` and `int`
fun errEnumAndInt(c: Color) {
    c == 1 && (c + 1);
}

// @stderr can not apply operator `+` to `int` and `string`
const errConstIntPlusString = 1 + "";

type MInt = int;

// @stderr can not apply operator `*` to `bool` and `MInt`
fun errBoolMulInt(c: cell) {
    var ten: MInt = 10;
    return (null == c) * ten;
}

// @stderr can not apply operator `~` to `bool`
fun errBitwiseNotOnBool() {
    var eq = 1 == 0;
    if (~eq) {
        return 0;
    }
    return -1;
}

// @stderr can not pass `int` to `builder`
fun errCallMethodNotSelf() {
    var test = builder.storeSlice(1, 5);
}

// @stderr can not assign `coins` to `slice`
// @stderr ton("0.04")
fun invalidInDefault(x: slice = ton("0.04")) {
}

// @stderr can not pass `(Point) -> (int, int)` to `slice`
// @stderr type `Point` can not be cast to `(int, int)`
fun passLambdaToPrimitiveAndMore() {
    beginCell().storeSlice(fun(p03: Point) { return p03 as (int, int) })
}

// @stderr can not apply operator `+` to `int8` and `string`
struct Gen<T> {
    price: coins = (1 as T) + "",
}

fun instantiateToInvalidPlus() {
    var c: Gen<int8> = {};
    return c;
}

// @stderr can not mutate a temporary expression
fun canNotMutateNonEmptyObject() {
    var (a, b) = (0, 0);
    Point {a, b}.assign();
}
