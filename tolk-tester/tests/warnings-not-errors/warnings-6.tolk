fun main() {
    // no warnings
    _ = blockchain.logicalTime();
    var ss = blockchain.configParam(10);
    blockchain.configParam(11) == null;

    // with warnings
    blockchain.now();
    createEmptySlice().loadRef();

    return 0;
}

/**
@testcase | 0 | | 0

@stderr result of a @pure function `blockchain.now` is unused
@stderr blockchain.now();
@stderr result of a @pure function `slice.loadRef` is unused

@stderr_avoid blockchain.logicalTime
@stderr_avoid blockchain.configParam
 */
