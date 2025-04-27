const C = match (10) { 10 => 0, else => -1 };

/**
@compilation_should_fail
@stderr not a constant expression
 */
