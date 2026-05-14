get fun seqno2(): int {
    return 0;
}

fun sharedFunction2(): int {
    return seqno2();
}

contract SomeContract2 {}
