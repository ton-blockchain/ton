
fun setTvmRegisterC7(c7: [tuple]): void
    asm "c7 POP";

@noinline
fun emulateC7PresentForTests(bounced: bool, senderAddress: address, fwdFee: coins, createdLt: int, createdAt: int, valueCoins: coins, valueExtra: dict) {
    var c7_inner = createEmptyTuple();
    repeat (17) {
        c7_inner.push(null);
    }

    var inmsgparams = createEmptyTuple();
    inmsgparams.push(null);        // bounce (not present in InMessage)
    inmsgparams.push(bounced);
    inmsgparams.push(senderAddress);
    inmsgparams.push(fwdFee);
    inmsgparams.push(createdLt);
    inmsgparams.push(createdAt);
    inmsgparams.push(null);        // orig value (not present in InMessage)
    inmsgparams.push(valueCoins);  // in onInternalMessage, from a stack; in onBouncedMessage, from TVM
    inmsgparams.push(valueExtra);
    inmsgparams.push(null);        // state init (not present in InMessage)
    c7_inner.push(inmsgparams);

    setTvmRegisterC7([c7_inner]);
}

fun invokeTest(body: slice, method_id: int): void
    asm "c3 PUSH" "EXECUTE";        // stack: body

@method_id(101)
fun handle1(in: InMessage) {
    return in.senderAddress.getWorkchain();
}

@method_id(201)
fun entrypoint1() {
    emulateC7PresentForTests(true, address("EQCtrtTXEAoSpoERmiqOnICe9LHxn2N89N4BH9qdHlrG-U0i"), 100, 20, 30, 900, null);
    invokeTest("", 101);
}

@method_id(102)
fun handle2(in: InMessage) {
    return (in.valueCoins, (in.senderAddress as any_address).isInternal());
}

@method_id(202)
fun entrypoint2() {
    emulateC7PresentForTests(false, address("EQCtrtTXEAoSpoERmiqOnICe9LHxn2N89N4BH9qdHlrG-U0i"), 100, 20, 30, 888, null);
    invokeTest("", 102);
}

@method_id(103)
fun handle3(`in ()`: InMessage) {
    return (
        `in ()`.senderAddress.getWorkchainAndHash(),
        `in ()`.valueCoins,
        `in ()`.valueExtra,
        `in ()`.createdLt,
        `in ()`.createdAt,
        `in ()`.body.isEmpty(),
    )
}

@method_id(203)
fun entrypoint3() {
    emulateC7PresentForTests(true, address("-1:000000000000000000000000000000000000000000000000000000000000FFFF"), 100, 20, 30, 999, null);
    invokeTest("00", 103);
}

fun onInternalMessage(in: InMessage) {
    __expect_type(in.body, "slice");
    __expect_type(in.senderAddress, "address");
    __expect_type(in.valueCoins, "coins");
    return in.originalForwardFee;
}

/**
@testcase | 201 |  | 0
@testcase | 202 |  | 888 -1
@testcase | 203 |  | -1 65535 999 (null) 20 30 0

@fif_codegen
"""
  handle1() PROC:<{             //  in.body
    DROP                        //
    INMSG_SRC                   //  '1
    REWRITESTDADDR
    DROP                        //  '3
  }>
"""

@fif_codegen
"""
  handle2() PROC:<{             //  in.body
    DROP                        //
    INMSG_VALUE                 //  '1
    INMSG_SRC                   //  '1 '3
    b{10} SDBEGINSQ
    NIP                         //  '1 '5
  }>
"""

@fif_codegen
"""
  handle3() PROC:<{
    INMSG_SRC
    REWRITESTDADDR
    INMSG_VALUE
    INMSG_VALUEEXTRA
    INMSG_LT
    INMSG_UTIME
    s0 s6 XCHG
    SEMPTY
    s5 s6 XCHG
    s4 s5 XCHG
    s3 s4 XCHG
    s1 s3 s0 XCHG3
  }>
"""

@fif_codegen
"""
  onInternalMessage() PROC:<{   //  in.body
    DROP
    INMSG_BOUNCED
    0 THROWIF
    INMSG_FWDFEE
    0 PUSHINT
    GETORIGINALFWDFEE
  }>
"""

 */
