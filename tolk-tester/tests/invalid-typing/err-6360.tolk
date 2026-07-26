fun autoInferIntOrString() {
    if ([0].get(0)) {
        return [1,2,3];
    }
    return ["aba"];
}

/**
@compilation_should_fail
@stderr function `autoInferIntOrString` calculated return type is `array<int> | array<string>`
 */
