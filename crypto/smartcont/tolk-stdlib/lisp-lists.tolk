// A part of standard library for Tolk
tolk 0.13

/**
    Lisp-style lists are nested 2-elements tuples: `[1, [2, [3, null]]]` represents list `[1, 2, 3]`.
    Elements of a list can be of different types.
    Empty list is conventionally represented as TVM `null` value.
 */

@pure
fun createEmptyList(): tuple
    asm "PUSHNULL";

/// Adds an element to the beginning of lisp-style list.
/// Note, that it does not mutate the list: instead, it returns a new one (it's a lisp pattern).
@pure
fun listPrepend<X>(head: X, tail: tuple?): tuple
    asm "CONS";

/// Extracts the head and the tail of lisp-style list.
@pure
fun listSplit<X>(list: tuple): (X, tuple?)
    asm "UNCONS";

/// Returns the head of lisp-style list.
@pure
fun listGetHead<X>(list: tuple): X
    asm "CAR";

/// Returns the tail of lisp-style list.
@pure
fun listGetTail(list: tuple): tuple?
    asm "CDR";
