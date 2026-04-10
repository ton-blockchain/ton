// @compilation_should_fail
// here are mixed errors from different compilation stages, to check they are fired all at once

enum Color { Red, Green, Blue }
const MY_CONST = 100;

// @stderr can not apply operator `+` to `Color` and `int`
fun errTypingOperator(c: Color) {
    c + 1;
}

// @stderr can not pass `string` to `Color`
fun errTypingArgument() {
    errTypingOperator("");
}

// @stderr can not assign `int` to a tensor
fun errTypingAssign() {
    var (x, y) = 2;
}

fun increment(mutate x: int) {
    x += 1;
}

// @stderr modifying immutable constant `MY_CONST`
// @stderr modifying immutable variable `v`
fun errSemanticsValConst() {
    val v = 10;
    v += 1;
    increment(mutate MY_CONST);
}

// @stderr can not mutate a temporary expression
fun errSemanticsLiteral() {
    increment(mutate 30);
}

type CustomType = (int, string);

// @stderr method `CustomType.packToBuilder` is declared incorrectly
fun CustomType.packToBuilder(self, b: builder) {  // missing `mutate`
}

fun CustomType.unpackFromSlice(mutate s: slice): CustomType {
    return 0;
}

fun errSerializationPack() {
    ((5, "") as CustomType).toCell();
}


fun add1(x: int) {
    return x + 1;
}

// @stderr `mutate` used for non-mutate parameter
fun callNonFunctionWithMutate() {
    val adder_fn = add1;
    var x = 10;
    return adder_fn(mutate x);
}

struct Point {
    x: int;
    y: int;
}

// @stderr auto-serialization via toCell() is not available for type `Point`
// @stderr because field `Point.x` of type `int` can't be serialized
fun serializeIntField(p: Point) {
    p.toCell();
}

type NotSerializableTensor = (int8, slice);

struct Demo {
    a: int8;
    b: int8 | int16;
    c: NotSerializableTensor;
}

// @stderr auto-serialization via fromSlice() is not available for type `Demo`
// @stderr struct `BigS` can exceed 1023 bits in serialization (estimated size: 1280..1280 bits)
fun serializeNotSerializableTensorAndBig() {
    Demo.fromSlice(createEmptySlice());
    WrapForBigS.fromSlice(createEmptySlice()).a.load();
}


struct BigS {
    d1: bits256;
    d2: bits256;
    d3: bits256;
    d4: bits256;
    d5: bits256;
}

struct WrapForBigS {
    a: Cell<BigS>;
}

// @stderr this method can not be declared for type `int`
fun int.packToBuilder(self, mutate b: builder) {
}

// @stderr union types are not allowed, use concrete types in `is`
fun unionTypesNotAllowedInIs(a: int | slice | builder) {
    if (a is int | slice) {}
}

type UserId = int
type OwnerId = int

// @stderr wrong pattern matching: `OwnerId` is not a variant of `UserId`
fun f(v: UserId) {
    match (v) {
        OwnerId => {}
    }
}

// @stderr invalid `map`: type `slice` can not be used as a key
// @stderr invalid `map`: type `int8?` can not be used as a key
// @stderr because its binary size is not constant: it's 1..9 bits
fun invalidMapKeyInsideCasts() {
    0 as unknown as map<slice, string> as unknown;

    var m = map<int8?, string> [];
    m.set(null, "");
    return m;
}
