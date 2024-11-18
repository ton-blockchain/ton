@method_id(1)
fun foo1(): int { return 111; }
@method_id(3)
fun foo2(): int { return 222; }
@method_id(10)
fun foo3(): int { return 333; }
fun main(): int { return 999; }

/**
    method_id | in | out
@testcase | 1  |    | 111
@testcase | 3  |    | 222
@testcase | 10 |    | 333
@testcase | 0  |    | 999
*/
