struct BigBodyMessage {
    v1: int256;
    v2: int256;
    v3: int256;
    a1: address?;
    a2: address?;
}

fun testFail(body: BigBodyMessage) {
    createExternalLogMessage({
        dest: createAddressNone(),
        body,
    });
}

/**
@compilation_should_fail
@stderr struct `BigBodyMessage` can exceed 1023 bits in serialization (estimated size: 772..1302 bits)
 */
