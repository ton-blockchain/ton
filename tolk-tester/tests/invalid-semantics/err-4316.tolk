struct Items {
    mapX: map<uint32, uint64>;
    mapY: map<uint64, bool>;
}

fun Items.addX(mutate self, k: uint32, v: uint64) {
    self.mapX.set(k, v);
}

fun Items.addY(mutate self, k: uint64, v: bool) {
    self.mapY.set(k, v);
}

fun Items.removeX(mutate self, i: uint32) {
    val lookup = self.mapX.deleteAndGetDeleted(i);
    assert(lookup.isFound) throw 444;
    return lookup.loadValue();
}

get fun test_dict() {
    var items = Items{
        mapX: createEmptyMap<uint32, uint64>(),
        mapY: createEmptyMap<uint64, bool>(),
    };

    items.addX(1, 2);
    items.addY(items.removeX(1), true);
    assert(!items.mapX.findFirst().isFound) throw 501;
}

/**
@compilation_should_fail
@stderr can not borrow `items` for mutation once again, it is already being mutated by `Items.addY`
 */
