import "../imports/imported-contract-3-invalid"

fun main(): int {
    return regularFunInC();
}

/**
@compilation_should_fail
@stderr has-get-fun.tolk:1:9
@stderr all contract entrypoints must be placed in the contract file `imported-contract-3-invalid.tolk`
@stderr get fun leakedGetter
 */
