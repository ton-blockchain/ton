import "../imports/imported-contract-1"

fun t(): int {
    return helperFromContract1() + seqno1();
}

/**
@compilation_should_fail
@stderr in function `t`
@stderr `seqno1` is a contract getter in `imported-contract-1.tolk` and is not accessible when imported
 */
