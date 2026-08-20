// line1
/* */ import "unexisting.tolk";
// line3

/**
@compilation_should_fail
On Linux/Mac, `realpath()` returns an error, and the error message is "cannot find file"
On Windows, it fails after, on reading, with a message "cannot open file"
@stderr error: Failed to import:
@stderr /err-2980.tolk:2:14
@stderr import "unexisting.tolk";
@stderr        ^^^^^^^^^^^^^^^^^
 */
