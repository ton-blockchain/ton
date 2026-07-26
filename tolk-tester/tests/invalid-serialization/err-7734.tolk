struct Storage {
    seqno: uint32;
}

fun Storage.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.seqno, 32);
}

fun Storage.unpackFromSlice(mutate s: slice): Storage {
    return Storage {
        seqno: s.loadUint(32),
    };
}

fun Storage.load() {
    return Storage.fromCell(contract.getData());
}

fun test(): int {
    val st = lazy Storage.load();
    return match (st) {
        Storage => st.seqno as int,
    };
}

/**
@compilation_should_fail
@stderr `lazy` is not applicable to `Storage`, because it overrides packToBuilder/unpackFromSlice
 */
