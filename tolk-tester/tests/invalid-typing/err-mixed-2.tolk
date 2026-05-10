// @compilation_should_fail

struct Point {
    x: int8
    y: int8
}

fun alwaysThrow(): never {
    throw 1;
}

// @stderr invalid left side of assignment
fun cantAssignToFunction(): void {
    cantAssignToFunction = cantAssignToFunction;
}

// @stderr can not assign `never` to `[...]`
fun cantAssignNeverToShape() {
    var [x] = alwaysThrow();
}

fun map<K, V>.my() {}
type MyBUilder = builder

// @stderr can not assign `string` to `int7`
fun asmFParamMismatch(x: int7 = "not an int"): int
    asm "INC";

// @stderr type `MyBUilder` can not be used as a value
fun invalidMapValueCustomCall() {
    map<int32, MyBUilder>.my();
}

type BadCoins = coins
type BadMap = map<BadCoins, int32>;

// @stderr type `BadCoins` can not be used as a key
fun aliasToInvalidMap() {
    var m: BadMap = [];
}

struct Set<T> {
    private m: map<T, ()>
}

// @stderr type `varuint32` can not be used as a key
fun invalidMapAfterGeneric(cc: Set<varuint32>) {
}

// @stderr can not apply operator `<<` to `never` and `never`
fun cantApplyMathToNever() {
    var p = lazy Point.fromSlice(createEmptySlice());
    match (p) {
        Point => {}
        else => { p << p }
    }
}

// @stderr invalid `map`: type `varint16` can not be used as a key
fun refToFunInvalidMap(): int {
    val f = createEmptyMap<varint16, int8>;
    var m = f();
}

// @stderr can not use `int` as a boolean condition
// @stderr can not use `coins` as a boolean condition
// @stderr can not use `int16` as a boolean condition
// @stderr hint: use not `assert (someNumber)` but `assert (someNumber != 0)`
// @stderr hint: use not `while (someNumber)` but `while (someNumber != 0)`
fun notBooleanCondition(i: int, c: coins, i16: int16) {
    assert (i) throw 123;
    do {} while (c);
    while (i16) {};
}

// @stderr can not apply operator `!` to `int`
// @stderr can not apply operator `!` to `coins`
// @stderr can not apply operator `!` to `int16`
// @stderr hint: use not `!someNumber` but `someNumber == 0`
fun notIntegerLogicalNot(i: int, c: coins, i16: int16) {
    var a = !i;
    var b = !c;
    var d = !i16;
}
