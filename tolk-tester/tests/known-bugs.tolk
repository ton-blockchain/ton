fun increment(mutate x: int): int {
    x = x + 1;
    return x;
}

@method_id(101)
fun bugWithModifyingMethodInsideSameExpression() {
    /*
        The same bug existed in FunC:
#pragma allow-post-modification;
(int, int) ~increment(int x) { x = x + 5; return (x, x); }
int main() { int x = 0; x += x~increment(); return x; }
        It's related to using a variable modified by ~method inside the same expression.
    */
    var x = 0;
    x = x + increment(mutate x);
    return x;
}

fun main() {

}

/**
// correct: 2
@testcase | 101 | | 1
 */
