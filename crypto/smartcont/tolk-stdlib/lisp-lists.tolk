// A part of standard library for Tolk
tolk 1.3

/**
    A lisp-style list is a set of nested 2-element TVM tuples:
    `[1, [2, [3, null]]]` represents a list `[1, 2, 3]`.
    Unlike `array<T>`, it can represent a list longer than 255 elements,
    but it has access only to the frontmost element (head).

    To create a lisp list, use `[]` with a hint:
    ```
    var emptyList: lisp_list<int> = [];
    // or
    var emptyList = [] as lisp_list<int>;
    ```

    Creating non-empty lists is also possible:
    ```
    var numsList = [1, 2, 3] as lisp_list<int>;
    // stack: [1 [2 [3 null]]]
    ```
 */

@pure
fun lisp_list<T>.isEmpty(self): bool {
    return self.tvmTuple == null
}

/// Inserts an element an the front.
/// Before: `[a b c]`.
/// After: `[new a b c]`.
@pure
fun lisp_list<T>.prependHead(mutate self, value: T): void {
    self.tvmTuple = [value, self.tvmTuple as unknown as lisp_list<T>];
}

/// Pops an element from the front and returns it.
/// Before: `[a b c]`.
/// After: `[b c]`, returns `a`.
/// Throws an exceptions if a list is empty.
@pure
fun lisp_list<T>.popHead(mutate self): T {
    var [head, tail] = self.tvmTuple!;
    self.tvmTuple = tail as unknown as [T, lisp_list<T>]?;
    return head
}

/// Returns an element from the front: `a` for `[a b c]`.
/// Throws an exceptions if a list is empty.
@pure
fun lisp_list<T>.getHead(self): T {
    return self.tvmTuple!.0
}

/// Returns all but the frontmost element: `[b c]` for `[a b c]`.
/// Throws an exceptions if a list is empty.
@pure
fun lisp_list<T>.getTail(self): lisp_list<T> {
    return self.tvmTuple!.1
}

/// Calculates the size of a list by looping all the elements (there is no other way to do it).
/// The longer the list — the more gas consumption (O(N) complexity).
@pure
fun lisp_list<T>.calculateSize(self): int {
    var size = 0;
    var ptr = self;
    while (!ptr.isEmpty()) {
        ptr = ptr.getTail();
        size += 1;
    }
    return size;
}

/// Returns the list with elements in reversed order.
/// Given `[a b c]` returns `[c b a]`.
@pure
fun lisp_list<T>.calculateReversed(self): lisp_list<T> {
    var reversed = [] as lisp_list<T>;
    var ptr = self;
    while (!ptr.isEmpty()) {
        reversed.prependHead(ptr.popHead());
    }
    return reversed;
}

/// Returns the concatenation of two lists.
/// Given `[a b]` and `[c d]`, returns `[a b c d]`.
@pure
fun lisp_list<T>.calculateConcatenation(self, other: lisp_list<T>): lisp_list<T> {
    var reversed = [] as lisp_list<T>;
    var ptr = self;
    while (!ptr.isEmpty()) {
        reversed.prependHead(ptr.popHead());
    }
    ptr = other;
    while (!ptr.isEmpty()) {
        reversed.prependHead(ptr.popHead());
    }
    return reversed.calculateReversed();
}

/// Creates a lisp_list<T> with zero elements.
/// Deprecated. Use `[]` syntax instead:
/// ```
/// // old way, deprecated:
/// var list = createEmptyList<int>();
///
/// // new way, preferred:
/// var t = lisp_list<int> [];
/// ```
@pure
@deprecated("use `[]` instead of `createEmptyList`")
fun createEmptyList<T>(): lisp_list<T>
    asm "PUSHNULL"


/// `lisp_list<T>` can be serialized if `T` is serializable. It's encoded as reversed snake refs:
/// A list `[1 2 3]` (on a stack: `[1 [2 [3 null]]]`) is "3.ref(2.ref(1.ref(empty cell)))".
/// An empty list `[]` (on a stack: `null`) is "empty cell"
fun lisp_list<T>.packToBuilder(self, mutate b: builder): void {
    var tail = createEmptyCell();
    var ptr = self;
    while (!ptr.isEmpty()) {
        tail = beginCell().storeRef(tail).storeAny<T>(ptr.popHead()).endCell();
    }
    b.storeRef(tail);
}

/// Unserialize `lisp_list<T>` from snake refs:
/// From "3.ref(2.ref(1.ref(empty cell)))" get a list `[1 2 3]` (on a stack: `[1 [2 [3 null]]]`).
fun lisp_list<T>.unpackFromSlice(mutate s: slice): lisp_list<T> {
    var result = [] as lisp_list<T>;
    var head = s.loadRef().beginParse();
    while (!head.isEmpty()) {
        val tailSnaked = head.loadRef();
        val headValue = head.loadAny<T>();
        head.assertEnd();   // ensure no data is present besides T
        result.prependHead(headValue);
        head = tailSnaked.beginParse();
    }
    return result;
}
