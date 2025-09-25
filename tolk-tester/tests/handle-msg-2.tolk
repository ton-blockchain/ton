
@method_id(101)
fun test1() {
    return 0;
}

fun onInternalMessage(in: InMessage) {
    return in.valueExtra;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onInternalMessage() PROC:<{
    DROP
    INMSG_BOUNCED
    0 THROWIF
    INMSG_VALUEEXTRA
  }>
"""
*/