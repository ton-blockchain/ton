struct Point {
    x: int = 10
    y: int = 20
}

fun main() {
    // ok
    [] as [];
    [] as []?;
    [] as [] | int;
    [] as array<unknown>;
    [] as array<array<int>>;
    [] as array<array<int>>?;
    [] as array<array<int>> | int;
    [] as lisp_list<int>;
    [] as lisp_list<int>?;
    [] as lisp_list<int> | int;
    // err
    [] as Point | int;
}

/**
@compilation_should_fail
@stderr type `array<unknown>` can not be cast to `Point | int`
 */
