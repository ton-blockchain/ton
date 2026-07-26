import "../imports/imported-contract-2"

/**
@compilation_should_fail
@stderr in function `sharedFunction2`
@stderr `seqno2` is a contract getter in `imported-contract-2.tolk` and is not accessible when imported
@stderr return seqno2()
 */
