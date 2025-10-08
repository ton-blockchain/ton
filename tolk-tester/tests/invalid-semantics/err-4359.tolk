fun m() {
    var c = (12 as int64).toCell();
    var s = c.tvmCell.beginParse();
}

/**
@compilation_should_fail
@stderr field `Cell<int64>.tvmCell` is private
 */
