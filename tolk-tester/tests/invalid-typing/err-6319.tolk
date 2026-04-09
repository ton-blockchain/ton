fun main() {
    var arrUnknown = [];
    var arrInt = [5];
    var arrArrIntOk: array<array<int>> = [
        [1,2,3],
        [],
        arrInt,
        arrUnknown as array<int>
    ];
    var arrArrIntFail: array<array<int>> = [
        [1,2,3],
        [],
        arrInt,
        arrUnknown      // fail here
    ];
}

/**
@compilation_should_fail
@stderr can not convert `array<unknown>` to `array<int>`
@stderr fail here
 */
