struct Point {
    x: int = 10
    y: int = 20
}

fun main() {
    [] as [];                           // ok
    [] as []?;                          // ok
    [] as [] | int;                     // ok
    [] as array<unknown>;               // ok
    [] as array<array<int>>;            // ok
    [] as array<array<int>>?;           // ok
    [] as array<array<int>> | int;      // ok
    [] as lisp_list<int>;               // ok
    [] as lisp_list<int>?;              // ok
    [] as lisp_list<int> | int;         // ok
    // err
    [] as Point | int;
}

/**
@compilation_should_fail
@stderr type `array<unknown>` can not be cast to `Point | int`
@stderr_avoid ok
 */
