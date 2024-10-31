const MAX_SLIPAGE = 100;

fun main(jetton_amount: int, msg_value: int, slippage: int) {
    if ((0 == jetton_amount) | (msg_value == 0) | true | false | slippage > MAX_SLIPAGE) {
    }
}

/**
@compilation_should_fail
@stderr | has lower precedence than >
*/
