
@method_id(101)
fun test1() {
    return 0;
}

@on_bounced_policy("manual")
fun onInternalMessage(in: InMessage) {
    return in.valueExtra;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onInternalMessage() PROC:<{
    DROP
    INMSG_VALUEEXTRA
  }>
"""
*/