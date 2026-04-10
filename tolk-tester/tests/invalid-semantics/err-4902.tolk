struct MyMap<K, V>

fun MyMap<K, slice>.amb(self) { return 0 }
fun MyMap<int, V>.amb(self) { return 0 }

fun main() {
    MyMap<int8, slice>{}.amb();   // ok
    MyMap<int, slice?>{}.amb();   // ok

    MyMap<int, slice>{}.amb();
}

/**
@compilation_should_fail
@stderr call to method `amb` for type `MyMap<int, slice>` is ambiguous
@stderr candidate function: `MyMap<K,slice>.amb` with K=`int`
@stderr candidate function: `MyMap<int,V>.amb` with V=`slice`
 */
