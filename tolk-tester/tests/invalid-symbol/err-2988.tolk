import "../imports/imported-contract-4"

contract ContractB {}

get fun sharedValue4(): int {
    return 2;
}

fun main(): int {
    return sharedValue4();
}

/**
@compilation_should_fail
@stderr in function `valueViaContract4`
@stderr `sharedValue4` is a contract getter in `imported-contract-4.tolk` and is not accessible when imported
@stderr return sharedValue4()
 */
