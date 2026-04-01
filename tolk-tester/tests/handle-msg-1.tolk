
@method_id(101)
fun dummy() {
    return 0;
}

fun readN(n: int, mutate s: slice): int {
    return s.loadUint(n);
}

fun onInternalMessage(in: InMessage) {
    __expect_type(in.body, "slice");
    __expect_type(in.senderAddress, "address");
    __expect_type(in.valueCoins, "coins");
    readN(32, mutate in.body);
    return (in.originalForwardFee, in.body);
}

/**
@testcase | 101 |  | 0

@fif_codegen
"""
  onInternalMessage() PROC:<{   //  in.body
    INMSG_BOUNCED
    0 THROWIF
    32 LDU
    NIP
    INMSG_FWDFEE
    0 PUSHINT
    GETORIGINALFWDFEE
    SWAP                        //  '17 in.body
  }>
"""

 */
