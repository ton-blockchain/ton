// @compilation_should_fail

// @stderr field `Cell<int64>.tvmCell` is private
fun errUseFieldOfCellT() {
    var c = (12 as int64).toCell();
    var s = c.tvmCell.beginParse();
}

struct Point { private x: int, private y: int }
struct Point3d { x: int, y: int, z: int }

// @stderr field `Point.x` is private
fun Point3d.create() {
    return Point { x: 10, y: 20 }
}

struct Wrap<T> { private item: T }

// @stderr field `Wrap<int32>.item` is private
fun errUsePrivateInF(w: Wrap<int32>) {
    w = w;
    return w.item!;
}
