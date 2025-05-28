struct ExtraData {
    owner: address;
    lastTime: int;
}

struct Storage {
    more: Cell<ExtraData>;
}

fun main() {
    Storage.fromSlice("");
}

/**
@compilation_should_fail
@stderr error: auto-serialization via fromSlice() is not available for type `Storage`
@stderr because field `Storage.more` of type `Cell<ExtraData>` can't be serialized
@stderr because type `ExtraData` can't be serialized
@stderr because field `ExtraData.lastTime` of type `int` can't be serialized
@stderr because type `int` is not serializable, it doesn't define binary width
@stderr hint: replace `int` with `int32` / `uint64` / `coins` / etc.
 */
