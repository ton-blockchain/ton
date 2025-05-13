# Usage: `tolk-tester.py tests_dir` OR `tolk-tester.py tests_dir file_pattern`
# from current dir, providing some env (see getenv() calls).
# Every .tolk file should provide /* testcase description in a comment */, consider tests/ folder.
#
# Tests for Tolk can be
# * positive (compiled to .fif, run with fift, compared output with the one expected)
# * negative (compilation fails, and it's expected; patterns in stderr can be specified)
#
# Note, that there is also tolk-tester.js to test Tolk compiled to WASM.
# Don't forget to keep it identical to Python version!

import os
import os.path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import List


def getenv(name, default=None):
    if name in os.environ:
        return os.environ[name]
    if default is None:
        print("Environment variable", name, "is not set", file=sys.stderr)
        exit(1)
    return default


TOLK_EXECUTABLE = getenv("TOLK_EXECUTABLE", "tolk")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
FIFT_LIBS_FOLDER = getenv("FIFTPATH")  # this env is needed for fift to work properly
TMP_DIR = tempfile.mkdtemp()


class CmdLineOptions:
    def __init__(self, argv: List[str]):
        if len(argv) < 2:
            print("Usage: tolk-tester.py tests_dir [file_pattern]", file=sys.stderr)
            exit(1)
        if not os.path.isdir(argv[1]):
            print("Directory '%s' doesn't exist" % argv[1], file=sys.stderr)
            exit(1)

        self.tests_dir = argv[1]
        self.file_pattern = argv[2] if len(argv) > 2 else None

    def find_tests(self) -> List[str]:
        all_test_files: List[str] = []
        all_children_of_tests_dir = os.listdir(self.tests_dir)
        all_children_of_tests_dir.sort()
        for f in all_children_of_tests_dir:
            if f.endswith(".tolk"):
                all_test_files.append(os.path.join(self.tests_dir, f))
        for f in all_children_of_tests_dir:
            if not f.endswith(".tolk") and f != "imports":
                subdir = os.path.join(self.tests_dir, f)
                children_of_subdir = os.listdir(subdir)
                children_of_subdir.sort()
                all_test_files += [os.path.join(subdir, f) for f in children_of_subdir]

        if self.file_pattern is not None:
            all_test_files = [f for f in all_test_files if f.find(self.file_pattern) != -1]
        return all_test_files


class ParseInputError(Exception):
    pass


class TolkCompilationFailedError(Exception):
    def __init__(self, message: str, stderr: str):
        super().__init__(message)
        self.stderr = stderr


class TolkCompilationSucceededError(Exception):
    pass


class FiftExecutionFailedError(Exception):
    def __init__(self, message: str, stderr: str):
        super().__init__(message)
        self.stderr = stderr


class CompareOutputError(Exception):
    def __init__(self, message: str, output: str):
        super().__init__(message)
        self.output = output


class CompareFifCodegenError(Exception):
    pass


class CompareCodeHashError(Exception):
    pass


class TolkTestCaseInputOutput:
    """
    In positive tests, there are several testcases "input X should produce output Y".
    They are written as a table:
    @testcase | method_id | input (one or several) | output
    """
    reJustNumber = re.compile(r"[-+]?\d+")
    reMathExpr = re.compile(r"[0x123456789()+\-*/<>]+")
    reGasUsed = re.compile(r"gas:\sused=(\d+)")

    def __init__(self, method_id_str: str, input_str: str, output_str: str):
        processed_inputs = []
        for in_arg in input_str.split(" "):
            if len(in_arg) == 0:
                continue
            elif in_arg.startswith("x{") or TolkTestCaseInputOutput.reJustNumber.fullmatch(in_arg):
                processed_inputs.append(in_arg)
            elif TolkTestCaseInputOutput.reMathExpr.fullmatch(in_arg):
                processed_inputs.append(str(eval(in_arg)))
            elif in_arg == "null":
                processed_inputs.append("null")
            else:
                raise ParseInputError("'%s' can't be evaluated" % in_arg)

        self.method_id = int(method_id_str)
        self.input = " ".join(processed_inputs)
        self.expected_output = output_str

    def check(self, stdout_lines: List[str], line_idx: int):
        if stdout_lines[line_idx] != self.expected_output:
            raise CompareOutputError("error on case #%d (%d | %s):\n    expect: %s\n    actual: %s" % (line_idx + 1, self.method_id, self.input, self.expected_output, stdout_lines[line_idx]), "\n".join(stdout_lines))


class TolkTestCaseStderr:
    """
    @stderr checks, when compilation fails, that stderr (compilation error) is expected.
    If it's multiline, all lines must be present in specified order.
    """

    def __init__(self, stderr_pattern: List[str], avoid: bool):
        self.stderr_pattern = stderr_pattern
        self.avoid = avoid

    def check(self, stderr: str):
        line_match = self.find_pattern_in_stderr(stderr.splitlines())
        if line_match == -1 and not self.avoid:
            raise CompareOutputError("pattern not found in stderr:\n%s" %
                                     "\n".join(map(lambda x: "    " + x, self.stderr_pattern)), stderr)
        elif line_match != -1 and self.avoid:
            raise CompareOutputError("pattern found (line %d), but not expected to be:\n%s" %
                                     (line_match + 1, "\n".join(map(lambda x: "    " + x, self.stderr_pattern))), stderr)

    def find_pattern_in_stderr(self, stderr: List[str]) -> int:
        for line_start in range(len(stderr)):
            if self.try_match_pattern(0, stderr, line_start):
                return line_start
        return -1

    def try_match_pattern(self, pattern_offset: int, stderr: List[str], offset: int) -> bool:
        if pattern_offset >= len(self.stderr_pattern):
            return True
        if offset >= len(stderr):
            return False

        line_pattern = self.stderr_pattern[pattern_offset]
        line_output = stderr[offset]
        return line_output.find(line_pattern) != -1 and self.try_match_pattern(pattern_offset + 1, stderr, offset + 1)


class TolkTestCaseFifCodegen:
    """
    @fif_codegen checks that contents of compiled.fif matches the expected pattern.
    @fif_codegen_avoid checks that is does not match the pattern.
    The pattern is a multiline piece of fift code, optionally with "..." meaning "any lines here".
    See tests/codegen_check_demo.tolk of how it looks.
    A notable thing about indentations (spaces at line starts):
    Taking them into account will complicate the code without reasonable profit,
    that's why we just trim every string.
    And one more word about //comments. Tolk inserts them into fift output.
    If a line in the pattern contains a //comment, it's expected to be equal.
    If a line does not, we just compare a command.
    """

    def __init__(self, fif_pattern: List[str], avoid: bool):
        self.fif_pattern = [s.strip() for s in fif_pattern]
        self.avoid = avoid

    def check(self, fif_output: List[str]):
        line_match = self.find_pattern_in_fif_output(fif_output)
        if line_match == -1 and not self.avoid:
            raise CompareFifCodegenError("pattern not found:\n%s" %
                                         "\n".join(map(lambda x: "    " + x, self.fif_pattern)))
        elif line_match != -1 and self.avoid:
            raise CompareFifCodegenError("pattern found (line %d), but not expected to be:\n%s" %
                                         (line_match + 1, "\n".join(map(lambda x: "    " + x, self.fif_pattern))))

    def find_pattern_in_fif_output(self, fif_output: List[str]) -> int:
        for line_start in range(len(fif_output)):
            if self.try_match_pattern(0, fif_output, line_start):
                return line_start
        return -1

    def try_match_pattern(self, pattern_offset: int, fif_output: List[str], offset: int) -> bool:
        if pattern_offset >= len(self.fif_pattern):
            return True
        if offset >= len(fif_output):
            return False
        line_pattern = self.fif_pattern[pattern_offset]
        line_output = fif_output[offset]

        if line_pattern != "...":
            if not TolkTestCaseFifCodegen.does_line_match(line_pattern, line_output):
                return False
            return self.try_match_pattern(pattern_offset + 1, fif_output, offset + 1)
        while offset < len(fif_output):
            if self.try_match_pattern(pattern_offset + 1, fif_output, offset):
                return True
            offset = offset + 1
        return False

    @staticmethod
    def split_line_to_cmd_and_comment(trimmed_line: str) -> tuple:
        pos = trimmed_line.find("//")
        if pos == -1:
            return trimmed_line, None
        else:
            return trimmed_line[:pos].rstrip(), trimmed_line[pos + 2:].lstrip()

    @staticmethod
    def does_line_match(line_pattern: str, line_output: str) -> bool:
        cmd_pattern, comment_pattern = TolkTestCaseFifCodegen.split_line_to_cmd_and_comment(line_pattern)
        cmd_output, comment_output = TolkTestCaseFifCodegen.split_line_to_cmd_and_comment(line_output.strip())
        return cmd_pattern == cmd_output and (comment_pattern is None or comment_pattern == comment_output)


class TolkTestCaseExpectedHash:
    """
    @code_hash checks that hash of compiled output.fif matches the provided value.
    It's used to "record" code boc hash and to check that it remains the same on compiler modifications.
    Being much less flexible than @fif_codegen, it nevertheless gives a guarantee of bytecode stability.
    """

    def __init__(self, expected_hash: str):
        self.code_hash = expected_hash

    def check(self, fif_code_hash: str):
        if self.code_hash != fif_code_hash:
            raise CompareCodeHashError("expected %s, actual %s" % (self.code_hash, fif_code_hash))


class TolkTestFile:
    def __init__(self, tolk_filename: str, artifacts_folder: str):
        self.line_idx = 0
        self.tolk_filename = tolk_filename
        self.artifacts_folder = artifacts_folder
        self.compilation_should_fail = False
        self.stderr_includes: List[TolkTestCaseStderr] = []
        self.input_output: List[TolkTestCaseInputOutput] = []
        self.fif_codegen: List[TolkTestCaseFifCodegen] = []
        self.expected_hash: TolkTestCaseExpectedHash | None = None
        self.experimental_options: str | None = None
        self.enable_tolk_lines_comments = False

    def parse_input_from_tolk_file(self):
        with open(self.tolk_filename, "r") as fd:
            lines = fd.read().splitlines()
        self.line_idx = 0

        while self.line_idx < len(lines):
            line = lines[self.line_idx]
            if line.startswith("@testcase"):
                s = [x.strip() for x in line.split("|")]
                if len(s) != 4:
                    raise ParseInputError("incorrect format of @testcase: %s" % line)
                self.input_output.append(TolkTestCaseInputOutput(s[1], s[2], s[3]))
            elif line.startswith("@compilation_should_fail"):
                self.compilation_should_fail = True
            elif line.startswith("@stderr"):
                self.stderr_includes.append(TolkTestCaseStderr(self.parse_string_value(lines), False))
            elif line.startswith("@fif_codegen_avoid"):
                self.fif_codegen.append(TolkTestCaseFifCodegen(self.parse_string_value(lines), True))
            elif line.startswith("@fif_codegen_enable_comments"):
                self.enable_tolk_lines_comments = True
            elif line.startswith("@fif_codegen"):
                self.fif_codegen.append(TolkTestCaseFifCodegen(self.parse_string_value(lines), False))
            elif line.startswith("@code_hash"):
                self.expected_hash = TolkTestCaseExpectedHash(self.parse_string_value(lines, False)[0])
            elif line.startswith("@experimental_options"):
                self.experimental_options = line[22:]
            self.line_idx = self.line_idx + 1

        if len(self.input_output) == 0 and not self.compilation_should_fail:
            raise ParseInputError("no @testcase present")
        if len(self.input_output) != 0 and self.compilation_should_fail:
            raise ParseInputError("@testcase present, but compilation_should_fail")

    def parse_string_value(self, lines: List[str], allow_multiline = True) -> List[str]:
        # a tag must be followed by a space (single-line), e.g. '@stderr some text'
        # or be a multi-line value, surrounded by """
        line = lines[self.line_idx]
        pos_sp = line.find(' ')
        is_multi_line = lines[self.line_idx + 1] == '"""'
        is_single_line = pos_sp != -1
        if not is_single_line and not is_multi_line:
            raise ParseInputError('%s value is empty (not followed by a string or a multiline """)' % line)
        if is_single_line and is_multi_line:
            raise ParseInputError('%s value is both single-line and followed by """' % line[:pos_sp])
        if is_multi_line and not allow_multiline:
            raise ParseInputError("%s value should be single-line" % line)

        if is_single_line:
            return [line[pos_sp + 1:].strip()]

        self.line_idx += 2
        s_multiline = []
        while self.line_idx < len(lines) and lines[self.line_idx] != '"""':
            s_multiline.append(lines[self.line_idx])
            self.line_idx = self.line_idx + 1
        return s_multiline

    def get_compiled_fif_filename(self):
        return self.artifacts_folder + "/compiled.fif"

    def get_runner_fif_filename(self):
        return self.artifacts_folder + "/runner.fif"

    def run_and_check(self):
        cmd_args = [TOLK_EXECUTABLE, "-o", self.get_compiled_fif_filename()]
        if self.experimental_options:
            cmd_args = cmd_args + ["-x", self.experimental_options]
        if not self.enable_tolk_lines_comments:
            cmd_args = cmd_args + ["-L"]
        res = subprocess.run(cmd_args + [self.tolk_filename], capture_output=True, timeout=10)
        exit_code = res.returncode
        stderr = str(res.stderr, "utf-8")
        stdout = str(res.stdout, "utf-8")

        if exit_code == 0 and self.compilation_should_fail:
            raise TolkCompilationSucceededError("compilation succeeded, but it should have failed")

        for should_include in self.stderr_includes: # @stderr is used to check errors and warnings
            should_include.check(stderr)

        if exit_code != 0 and self.compilation_should_fail:
            return 0

        if exit_code != 0 and not self.compilation_should_fail:
            raise TolkCompilationFailedError("tolk exit_code = %d" % exit_code, stderr)

        with open(self.get_runner_fif_filename(), "w") as fd:
            fd.write("\"%s\" include <s constant code\n" % self.get_compiled_fif_filename())
            for t in self.input_output:
                fd.write("%s %d code 1 runvmx abort\"exitcode is not 0\" .s cr { drop } depth 1- times\n" % (t.input, t.method_id))
            if self.expected_hash is not None:
                fd.write("\"%s\" include hash .s\n" % self.get_compiled_fif_filename())

        res = subprocess.run([FIFT_EXECUTABLE, self.get_runner_fif_filename()], capture_output=True, timeout=10)
        exit_code = res.returncode
        stderr = str(res.stderr, "utf-8")
        stdout = str(res.stdout, "utf-8")
        if exit_code != 0:
            raise FiftExecutionFailedError("fift exit_code = %d" % exit_code, stderr)

        gas_used = sum(map(int, TolkTestCaseInputOutput.reGasUsed.findall(stderr)))
        stdout_lines = [x.strip() for x in stdout.split("\n")]
        stdout_lines = [x for x in stdout_lines if x != ""]
        fif_code_hash = None
        if self.expected_hash is not None:  # then the last stdout line is a hash
            fif_code_hash = stdout_lines[-1]
            stdout_lines = stdout_lines[:-1]

        if len(stdout_lines) != len(self.input_output):
            raise CompareOutputError("unexpected number of fift output: %d lines, but %d testcases" % (len(stdout_lines), len(self.input_output)), stdout)

        for i in range(len(stdout_lines)):
            self.input_output[i].check(stdout_lines, i)

        if len(self.fif_codegen):
            with open(self.get_compiled_fif_filename()) as fd:
                fif_output = fd.readlines()
            for fif_codegen in self.fif_codegen:
                fif_codegen.check(fif_output)

        if self.expected_hash is not None:
            self.expected_hash.check(fif_code_hash)

        return gas_used


def run_all_tests(tests: List[str]):
    total_gas_used = 0
    for ti in range(len(tests)):
        tolk_filename = tests[ti]
        print("Running test %d/%d: %s" % (ti + 1, len(tests), os.path.basename(tolk_filename)), file=sys.stderr)

        artifacts_folder = os.path.join(TMP_DIR, tolk_filename)
        testcase = TolkTestFile(tolk_filename, artifacts_folder)
        try:
            if not os.path.exists(artifacts_folder):
                os.makedirs(artifacts_folder)
            testcase.parse_input_from_tolk_file()
            gas_used = testcase.run_and_check()
            shutil.rmtree(artifacts_folder)
            total_gas_used += gas_used

            if testcase.compilation_should_fail:
                print("  OK, stderr match", file=sys.stderr)
            else:
                print("  OK, %d cases" % (len(testcase.input_output)), file=sys.stderr)
        except ParseInputError as e:
            print("  Error parsing input (cur line #%d):" % (testcase.line_idx + 1), e, file=sys.stderr)
            exit(2)
        except TolkCompilationFailedError as e:
            print("  Error compiling tolk:", e, file=sys.stderr)
            print("  stderr:", file=sys.stderr)
            print(e.stderr.rstrip(), file=sys.stderr)
            exit(2)
        except TolkCompilationSucceededError as e:
            print("  Error:", e, file=sys.stderr)
            exit(2)
        except FiftExecutionFailedError as e:
            print("  Error executing fift:", e, file=sys.stderr)
            print("  stderr:", file=sys.stderr)
            print(e.stderr.rstrip(), file=sys.stderr)
            print("  compiled.fif at:", testcase.get_compiled_fif_filename(), file=sys.stderr)
            exit(2)
        except CompareOutputError as e:
            print("  Mismatch in output:", e, file=sys.stderr)
            print("  Full output:", file=sys.stderr)
            print(e.output.rstrip(), file=sys.stderr)
            print("  Was compiled to:", testcase.get_compiled_fif_filename(), file=sys.stderr)
            exit(2)
        except CompareFifCodegenError as e:
            print("  Mismatch in fif codegen:", e, file=sys.stderr)
            print("  Was compiled to:", testcase.get_compiled_fif_filename(), file=sys.stderr)
            print(open(testcase.get_compiled_fif_filename()).read(), file=sys.stderr)
            exit(2)
        except CompareCodeHashError as e:
            print("  Mismatch in code hash:", e, file=sys.stderr)
            print("  Was compiled to:", testcase.get_compiled_fif_filename(), file=sys.stderr)
            exit(2)
    return total_gas_used


tests = CmdLineOptions(sys.argv).find_tests()
print("Found", len(tests), "tests", file=sys.stderr)
total_gas_used = run_all_tests(tests)
print("Done, %d tests, gas %d" % (len(tests), total_gas_used), file=sys.stderr)
