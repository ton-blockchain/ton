// @compilation_should_fail

fun eq<T>(x: T) { return x }

fun int.add(mutate self, dx: int) {
    self += dx
}

fun int.del(mutate self): int {
    self = 0;
    return self
}

fun int.incSelfWithOther(mutate self, mutate other: int) {
    self += 1;
    other += 1;
}

// @stderr can not borrow `x001` for mutation once again, it is already being mutated by `int.add`
// @stderr hint: split a complex expression into several simple ones
fun errCantBorrow1() {
    var x001 = 10;
    x001.add(x001.del());
}


struct Nested {
    n1: int
    n2: int
}

struct Demo {
    a: int
    b: int
    c: int
    n1: Nested
    n2: Nested
}

fun Demo.assignB(mutate self, newB: int): Demo {
    self.b = newB;
    return self;
}

fun mutateDemoAndGetC(mutate d: Demo): int {
    return d.c
}

fun Nested.mut(mutate self, x: int): int {
    return x
}

fun Nested.assign(mutate self, n: Nested) {
    self = n
}


// @stderr can not borrow `d.n1.n2` for mutation, because `d.n1` is already being mutated by `Nested.mut`
// @stderr ^^^^^^^
fun errCantBorrow2WithOperator(d: Demo) {
    d.n1.mut(d.n1!.n2.del())
}

// @stderr can not borrow `x003` for mutation once again, it is already being mutated by `int.incSelfWithOther`
// @stderr_avoid y003
fun errCantBorrowReceiverAndMutate() {
    var x003 = 0;
    var y003 = 0;
    x003.incSelfWithOther(mutate y003);       // ok
    x003.incSelfWithOther(mutate x003);
}

global d13: Demo;

// @stderr can not borrow `d13` for mutation once again, it is already being mutated by `Demo.assignB`
// @stderr_avoid // ok
fun errCantBorrowWithGlobal(d2: Demo) {
    d13.assignB(10);                                  // ok
    d13.assignB(mutateDemoAndGetC(mutate d2));        // ok
    d13.assignB(d2.a += d13.b);                       // ok
    d13.assignB(d2.c = d13.n1.n1);                    // ok

    d13.assignB(mutateDemoAndGetC(mutate d13));
}

// @stderr can not borrow `n73.0` for mutation, because `n73` is already being mutated by `assignment operator`
fun errCantBorrowInsideNestedAssign() {
    var n73 = (1, 2);
    (n73, _, (_, n73.0)) = (n73, 0, (0, n73.1));
}

// @stderr can not borrow `d04.n1.n2` for mutation, because `d04.n1` is already being mutated by `Nested.assign`
// @stderr_avoid // ok
fun errCantBorrowNestedAssignment(d04: Demo) {
    d04.a += (d04.b += (d04.c = 10));       // ok
    d04.n1.assign({                         // ok
        n1: d04.n1.n1,                      // ok
        n2: d04.n1.n2,                      // ok
    });                                     // ok
    d04.n1.assign({
        n1: ((_, (d04).n1.n2) = (0, 1)).0,
        n2: 10,
    });
}

fun mutateOverlapping(mutate x: int, mutate p: (int, int)): void {
    x += 1;
    p = (3, 4);
}

// @stderr can not borrow `a19` for mutation once again, it is already being mutated by `mutateOverlapping`
fun errCantBorrowInsideTensor(): (int, int) {
    var a19 = 10;
    var b19 = 20;
    mutateOverlapping(mutate a19, mutate (((a19, b19))));
    return (a19, b19);
}


// @stderr can not borrow `x89` for mutation once again, it is already being mutated by `int.add`
// @stderr ([y, x89] = [0, 0]).0
// @stderr_avoid // ok
fun errCantBorrowInSquareAssign(x89: int, y: int) {
    x89 = (x89 += 10);                                // ok
    x89 += (y = x89);                                 // ok
    x89.add(([y] = [x89]).0);                     // ok
    x89.add(([y] = [x89]).0);                     // ok

    x89.add(([y, x89] = [0, 0]).0);
}

global xy5: (int, int);

// @stderr can not borrow `xy5.0` for mutation once again, it is already being mutated by `int.add`
fun errCantBorrowInVeryComplex() {
    xy5 = (0, 0);                                            // ok
    xy5.0 = (xy5.0 += 10);                                    // ok
    xy5.0 += (xy5.1 = xy5.0);                                  // ok
    xy5.0.add(([xy5.1] = [xy5.0]).0);                      // ok
    xy5.0.add(([xy5.1] = [xy5.0]).0);                      // ok

    var eqVoid = eq<void>;
    eqVoid(xy5.0.add(([xy5.1, (xy5.0.add(1), xy5.0).1] = [0, 0]).0));
}


struct Point {
    x: int
    y: int
}

fun Point.copy(self) { return self }
fun Point.mut(mutate self) { return self }

// @stderr can not assign to a temporary expression
// @stderr p03.mut().x
fun cantBorrowAfterReturnMutateSelf(p03: Point) {
    p03.x += (p03.mut().x = 5);
}

struct JustX { x: int }

fun JustX.id(self): self { return self; }
fun JustX.bump(mutate self) { self.x += 1; }

// @stderr can not mutate a temporary expression
// @stderr (a19.0 = JustX{x: 10}).id()
fun cantBorrowWithSelfFunction() {
    var a19: [JustX] = [JustX { x: 0 }];
    (a19.0 = JustX{x: 10}).id().bump();
}

global t107: (int, int);

fun (int, int).increment(mutate self) {
    self.0 += 1;
    self.1 += 1;
}

// @stderr can not borrow `x33` for mutation once again, it is already being mutated by `(int,int).increment`
fun errCantBorrowViaTensorIndex() {
    var x33 = 0;
    ((x33, x33).0, (x33, x33).1).increment();
}
