# Usage: `run_tests.py tests_dir` OR `run_tests.py test_file.fc`
# from current dir, providing some env (see getenv() calls).
# Every .fc file should provide {- testcase description in a comment -}, consider tests/ folder.
#
# Tests for FunC can be
# * positive (compiled to .fif, run with fift, compared output with the one expected)
# * negative (compilation fails, and it's expected; patterns in stderr can be specified)
#
# Note, that there is also run_tests.js to test FunC compiled to WASM.
# Don't forget to keep it identical to Python version!

import os
import os.path
import re
import shutil
import subprocess
import sys
import tempfile


def getenv(name, default=None):
    if name in os.environ:
        return os.environ[name]
    if default is None:
        print("Environment variable", name, "is not set", file=sys.stderr)
        exit(1)
    return default


FUNC_EXECUTABLE = getenv("FUNC_EXECUTABLE", "func")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
FIFT_LIBS_FOLDER = getenv("FIFTPATH")  # this env is needed for fift to work properly
TMP_DIR = tempfile.mkdtemp()


class CmdLineOptions:
    def __init__(self, argv: list[str]):
        if len(argv) != 2:
            print("Usage: run_tests.py tests_dir OR run_tests.py test_file.fc", file=sys.stderr)
            exit(1)
        if not os.path.exists(argv[1]):
            print("Input '%s' doesn't exist" % argv[1], file=sys.stderr)
            exit(1)

        if os.path.isdir(argv[1]):
            self.tests_dir = argv[1]
            self.test_file = None
        else:
            self.tests_dir = os.path.dirname(argv[1])
            self.test_file = argv[1]

    def find_tests(self) -> list[str]:
        if self.test_file is not None:  # an option to run (debug) a single test
            return [self.test_file]

        tests = [f for f in os.listdir(self.tests_dir) if f.endswith(".fc") or f.endswith(".func")]
        tests.sort()
        return [os.path.join(self.tests_dir, f) for f in tests]


class ParseInputError(Exception):
    pass


class FuncCompilationFailedError(Exception):
    def __init__(self, message: str, stderr: str):
        super().__init__(message)
        self.stderr = stderr


class FuncCompilationSucceededError(Exception):
    pass


class FiftExecutionFailedError(Exception):
    def __init__(self, message: str, stderr: str):
        super().__init__(message)
        self.stderr = stderr


class CompareOutputError(Exception):
    def __init__(self, message: str, output: str):
        super().__init__(message)
        self.output = output


class FuncTestCaseInputOutput:
    """
    In positive tests, there are several testcases "input X should produce output Y".
    They are written as a table:
    TESTCASE | method_id | input (one or several) | output
    """
    reJustNumber = re.compile("[-+]?\d+")
    reMathExpr = re.compile("[0x123456789()+\-*/<>]+")

    def __init__(self, method_id_str: str, input_str: str, output_str: str):
        processed_inputs = []
        for in_arg in input_str.split(" "):
            if len(in_arg) == 0:
                continue
            elif in_arg.startswith("x{") or FuncTestCaseInputOutput.reJustNumber.fullmatch(in_arg):
                processed_inputs.append(in_arg)
            elif FuncTestCaseInputOutput.reMathExpr.fullmatch(in_arg):
                processed_inputs.append(str(eval(in_arg)))
            else:
                raise ParseInputError("'%s' can't be evaluated" % in_arg)

        self.method_id = int(method_id_str)
        self.input = " ".join(processed_inputs)
        self.expected_output = output_str

    def check(self, stdout_lines: list[str], line_idx: int):
        if stdout_lines[line_idx] != self.expected_output:
            raise CompareOutputError("error on case %d: expected '%s', found '%s'" % (line_idx + 1, self.expected_output, stdout_lines[line_idx]), "\n".join(stdout_lines))


class FuncTestCaseStderrIncludes:
    """
    @stderr checks, when compilation fails, that stderr (compilation error) is expected.
    """

    def __init__(self, expected_substr: str):
        self.expected_substr = expected_substr

    def check(self, stderr: str):
        if self.expected_substr not in stderr:
            raise CompareOutputError("pattern '%s' not found in stderr" % self.expected_substr, stderr)


class FuncTestFile:
    def __init__(self, func_filename: str, artifacts_folder: str):
        self.func_filename = func_filename
        self.artifacts_folder = artifacts_folder
        self.compilation_should_fail = False
        self.stderr_includes: list[FuncTestCaseStderrIncludes] = []
        self.input_output: list[FuncTestCaseInputOutput] = []

    def parse_input_from_func_file(self):
        with open(self.func_filename, "r") as fd:
            lines = fd.read().splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            if line.startswith("TESTCASE"):
                s = [x.strip() for x in line.split("|")]
                if len(s) != 4:
                    raise ParseInputError("incorrect format of TESTCASE: %s" % line)
                self.input_output.append(FuncTestCaseInputOutput(s[1], s[2], s[3]))
            elif line.startswith("@compilation_should_fail"):
                self.compilation_should_fail = True
            elif line.startswith("@stderr"):
                self.stderr_includes.append(FuncTestCaseStderrIncludes(line[7:].strip()))
            i = i + 1

        if len(self.input_output) == 0 and not self.compilation_should_fail:
            raise ParseInputError("no TESTCASE present")
        if len(self.input_output) != 0 and self.compilation_should_fail:
            raise ParseInputError("TESTCASE present, but compilation_should_fail")

    def get_compiled_fif_filename(self):
        return self.artifacts_folder + "/compiled.fif"

    @property
    def get_runner_fif_filename(self):
        return self.artifacts_folder + "/runner.fif"

    def run_and_check(self):
        res = subprocess.run([FUNC_EXECUTABLE, "-o", self.get_compiled_fif_filename(), "-SPA", self.func_filename], capture_output=True, timeout=10)
        exit_code = res.returncode
        stderr = str(res.stderr, "utf-8")
        stdout = str(res.stdout, "utf-8")

        if exit_code == 0 and self.compilation_should_fail:
            raise FuncCompilationSucceededError("compilation succeeded, but it should have failed")

        if exit_code != 0 and self.compilation_should_fail:
            for should_include in self.stderr_includes:
                should_include.check(stderr)
            return

        if exit_code != 0 and not self.compilation_should_fail:
            raise FuncCompilationFailedError("func exit_code = %d" % exit_code, stderr)

        with open(self.get_runner_fif_filename, "w") as f:
            f.write("\"%s\" include <s constant code\n" % self.get_compiled_fif_filename())
            for t in self.input_output:
                f.write("%s %d code 1 runvmx abort\"exitcode is not 0\" .s cr { drop } depth 1- times\n" % (t.input, t.method_id))

        res = subprocess.run([FIFT_EXECUTABLE, self.get_runner_fif_filename], capture_output=True, timeout=10)
        exit_code = res.returncode
        stderr = str(res.stderr, "utf-8")
        stdout = str(res.stdout, "utf-8")
        stdout_lines = [x.strip() for x in stdout.split("\n")]
        stdout_lines = [x for x in stdout_lines if x != ""]

        if exit_code != 0:
            raise FiftExecutionFailedError("fift exit_code = %d" % exit_code, stderr)

        if len(stdout_lines) != len(self.input_output):
            raise CompareOutputError("unexpected number of fift output: %d lines, but %d testcases" % (len(stdout_lines), len(self.input_output)), stdout)

        for i in range(len(stdout_lines)):
            self.input_output[i].check(stdout_lines, i)


def run_all_tests(tests: list[str]):
    for ti in range(len(tests)):
        func_filename = tests[ti]
        print("Running test %d/%d: %s" % (ti + 1, len(tests), func_filename), file=sys.stderr)

        artifacts_folder = os.path.join(TMP_DIR, func_filename)
        testcase = FuncTestFile(func_filename, artifacts_folder)
        try:
            if not os.path.exists(artifacts_folder):
                os.makedirs(artifacts_folder)
            testcase.parse_input_from_func_file()
            testcase.run_and_check()
            shutil.rmtree(artifacts_folder)

            if testcase.compilation_should_fail:
                print("  OK, compilation failed as it should", file=sys.stderr)
            else:
                print("  OK, %d cases" % len(testcase.input_output), file=sys.stderr)
        except ParseInputError as e:
            print("  Error parsing input:", e, file=sys.stderr)
            exit(2)
        except FuncCompilationFailedError as e:
            print("  Error compiling func:", e, file=sys.stderr)
            print("  stderr:", file=sys.stderr)
            print(e.stderr.rstrip(), file=sys.stderr)
            exit(2)
        except FuncCompilationSucceededError as e:
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


tests = CmdLineOptions(sys.argv).find_tests()
print("Found", len(tests), "tests", file=sys.stderr)
run_all_tests(tests)
print("Done, %d tests" % len(tests), file=sys.stderr)
