struct Point {
    x: int = 10
    y: int = 20
}

fun main() {
    var sh: [int|builder, int|Point, int|Point|null, int|Point|null, int|(int,int,int)] =
            [beginCell(), 5, Point{}, null, 5];

    (sh.3 as unknown as (int, int)).0 = -888;
}

/**
@compilation_should_fail
@stderr can not assign to a temporary expression
@stderr -888
 */
