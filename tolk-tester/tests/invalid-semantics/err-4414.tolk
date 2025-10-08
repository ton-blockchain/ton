struct Point { private x: int, private y: int }
struct Point3d { x: int, y: int, z: int }

fun Point3d.create() {
    return Point { x: 10, y: 20 }
}

/**
@compilation_should_fail
@stderr field `Point.x` is private
 */
