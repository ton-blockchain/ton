// @compilation_should_fail
// various tests for type checker errors (not all! there are lots of independent err-xxxx)

fun f2args<T>(x: T, y: T) {}

// @stderr can not pass `string` to `int`
fun errPassStringToInt() {
    return f2args(32, "");
}

// @stderr can not pass `bool` to `int`
// @stderr hint: use `as` operator for UNSAFE casting
// @stderr caution! in TVM, bool TRUE is -1, not 1
fun errPassBoolToInt() {
    blockchain.configParam(true);
}

// @stderr can not pass `string` to `int`
// @stderr fIntInt(1, ( (((""))) ))
// @stderr            ^^^^^^^^^^^^
// (a whole argument is underlined, despite the fact that parentheses don't exist in AST)
fun errPassStringToIntWithParens() {
    fIntInt(1, ( (((""))) ));
}

fun fIntInt(a: int, i: int) {}

// @stderr can not pass `string` to `int32`
fun errPassStringToMap() {
    var m = createEmptyMap<int8, int32>();
    m.set(5, "");
}

struct A { id: int; value: int; }

fun takeTensor(v: (int, int)) {}

// @stderr can not pass `A` to `(int, int)`
fun errPassStructAsTensor() {
    takeTensor(A{id: 1, value: 2});
}

struct Pair<T1,T2> {
    first: T1;
    second: T2;
}

fun Pair<T1,T2>.demoInvalid(self) {
    return self.first == (null as int?);
}

fun Pair<AA,BB>.compareWith<U,V>(self, f: U, s: V) {
    return self.first == f && self.second == s;
}


// @stderr can not apply operator `==` to `int` and `int?`
// @stderr in function `Pair<int,slice>.demoInvalid`
// @stderr check on `null` first, or use unsafe operator `!`
fun instantiatePairAndCall(p: Pair<int,slice>) {
    p.demoInvalid();
}

// @stderr can not apply operator `==` to `address` and `address?`
// @stderr in function `Pair<address,slice>.compareWith<address?,slice?>`
// @stderr check on `null` first, or use unsafe operator `!`
fun callGenericMethodOnGenericReceiver(p: Pair<address,slice>) {
    p.compareWith<address?,slice?>(null, null);
}

// @stderr can not assign `address` to variable of type `any_address`
// @stderr hint: use `as` operator for conversion: `<some_expr> as any_address`
// @stderr can not assign `any_address` to variable of type `address`
// @stderr hint: use `any_addr.castToInternal()` to check and get `address`
// @stderr hint: alternatively, use `as` operator for UNSAFE casting: `<some_expr> as address`
fun incompatibleAddress(any: any_address, usual: address) {
    any = usual;
    usual = any;
}

// @stderr can not convert `int68` to `int32`
fun incompatibleSquareList() {
    lisp_list<int32> [1 as int, 3 as int32, 5 as int68];
}

fun autoGetIntOrNull() {
    if (random.uint256()) { return 1; }
    return null;
}

// @stderr type `int?` can not be cast to `builder`
fun testAutoInferredIntOrNull() {
    var b: builder = autoGetIntOrNull() as builder;
}
