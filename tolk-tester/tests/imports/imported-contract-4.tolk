import "../invalid-symbol/err-2988"

contract SomeContract4 {}

get fun sharedValue4(): int {
    return 1;
}

fun valueViaContract4(): int {
    return sharedValue4();
}
