struct MyMap<K, V>

fun MyMap<K, slice>.amb(self) { return 0 }
fun MyMap<int, V>.amb(self) { return 0 }

fun main() {
    MyMap<(int, int, MyMap<int, coins>), slice>{}.amb();   // ok
    MyMap<int, cell|()>{}.amb();   // ok

    MyMap<cell, builder>{}.amb();
}

/**
@compilation_should_fail
@stderr method `amb` not found for type `MyMap<cell, builder>`
@stderr (but it exists for type `MyMap<K, slice>`)
 */
