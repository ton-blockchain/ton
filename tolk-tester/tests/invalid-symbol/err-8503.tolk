const `Alias<int>` = 0;

type Alias<T> = T;

type TT = Alias<int>

/**
@compilation_should_fail
@stderr generic instantiation `Alias<int>` conflicts with existing symbol
 */
