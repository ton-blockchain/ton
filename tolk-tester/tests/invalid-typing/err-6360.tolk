fun autoInferIntOrSlice() {
    if ([0].get(0)) {
        return [1,2,3];
    }
    return ["aba"];
}

/**
@compilation_should_fail
@stderr function `autoInferIntOrSlice` calculated return type is `array<int> | array<slice>`
 */
