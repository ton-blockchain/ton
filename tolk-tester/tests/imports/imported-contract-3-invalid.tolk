// this gives compilation error, because it contains `get fun`: disallowed because of `contract` below
import "has-get-fun"

contract SomeContract3 {}

fun main(): int {
    return 2;
}

fun regularFunInC(): int {
    return 1;
}
