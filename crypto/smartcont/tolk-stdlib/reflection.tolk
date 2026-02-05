// A part of standard library for Tolk
tolk 1.2

/**
    Reflection API.
    `reflect` is a built-in struct with only static methods for compile-time type introspection.
 */

/// `reflect` is a built-in struct, it has only static methods.
/// Example: `reflect.typeNameOf<int>()` and other methods.
struct reflect

/// Get human-readable type name of any T for logging or low-level purposes.
/// Example:
/// ```
/// debug.printString(reflect.typeNameOf<int>());  // "int"
/// ```
@pure
fun reflect.typeNameOf<T>(): string
    builtin

/// Get human-readable type name of any object for logging or low-level purposes.
/// Example:
/// ```
/// debug.printString(reflect.typeNameOfObject(42));   // "int"
/// ```
@pure
fun reflect.typeNameOfObject<T>(anyVariable: T): string
    builtin

/// Returns the number of stack slots a type T occupies.
/// Example:
/// ```
/// reflect.stackSizeOf<int>()     // 1
/// reflect.stackSizeOf<(int, int)>()  // 2
/// ```
@pure
fun reflect.stackSizeOf<T>(): int
    builtin

/// Returns the number of stack slots any object occupies.
/// - for nullableInt it's 1, because `int?` is 1 TVM slot holding either NULL or a value.
/// - for somePoint it's 2 for `struct Point { x:int, y:int }`: two fields one slot per each.
/// Useful for debugging or when preparing stack contents for RUNVM.
@pure
fun reflect.stackSizeOfObject<T>(anyVariable: T): int
    builtin

/// Returns serialization prefix of a struct.
/// Example: for `struct (0xF0) AssetRegular { ... }` will return `(240, 8)`.
/// ```
/// reflect.serializationPrefixOf<AssetRegular>().1   // 8
/// ```
@pure
fun reflect.serializationPrefixOf<T>(): (int, int)
    builtin

/// Estimate serialization size of any type.
/// Returns (minBits, maxBits, minRefs, maxRefs).
/// Example:
/// ```
/// reflect.estimateSerializationOf<int8?>()     // (1 9 0 0)
/// ```
@pure
fun reflect.estimateSerializationOf<T>(): (int, int, int, int)
    builtin
