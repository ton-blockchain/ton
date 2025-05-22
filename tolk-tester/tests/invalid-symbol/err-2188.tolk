fun main() {
    beginCell()
        .storeAddressNone()
        .storeUint(3, 32)
        .storeUnexisting()
        .storeInt(1, 32)
        .endCell();
}

/**
@compilation_should_fail
@stderr method `storeUnexisting` not found for type `builder`
@stderr .storeUnexisting()
 */
