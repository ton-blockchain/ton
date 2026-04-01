struct ExtraData {
    owner: address;
    lastTime: int;
}

struct Storage {
    more: Cell<ExtraData>;
}

fun main() {
    // it's okay, the field 'more' is not checked here
    var st = Storage.fromSlice(createEmptySlice());
    // fails on a load attempt:
    st.more.load();
}

/**
@compilation_should_fail
@stderr st.more.load();
@stderr error: auto-serialization via load() is not available for type `ExtraData`
@stderr because field `ExtraData.lastTime` of type `int` can't be serialized
 */
