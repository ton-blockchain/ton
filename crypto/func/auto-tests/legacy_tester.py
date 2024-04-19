# Usage: `legacy_tests.py` from current dir, providing some env (see getenv() calls).
# Unlike run_tests.py, it launches tests from legacy_tests/ folder (which are real-world contracts)
# and checks that code hashes are expected (that contracts are compiled exactly the same way).
# In other words, it doesn't execute TVM, it just compiles fift to acquire a contract hash.
# In the future, we may merge these tests with regular ones (when the testing framework becomes richer).
# Note, that there is also legacy_tester.js to test FunC compiled to WASM.

import os
import os.path
import re
import subprocess
import sys
import tempfile
import shutil

add_pragmas = [] #["allow-post-modification", "compute-asm-ltr"];


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

COMPILED_FIF = os.path.join(TMP_DIR, "compiled.fif")
RUNNER_FIF = os.path.join(TMP_DIR, "runner.fif")

TESTS_DIR = "legacy_tests"


def load_legacy_tests_list(jsonl_filename: str) -> list[tuple[str, int]]:
    with open(jsonl_filename) as fd:
        contents = fd.read()
    results = re.findall('^\[\s*"(.*?)"\s*,\s*(.*?)\s*]', contents, re.MULTILINE)
    return list(map(lambda line: (line[0], int(line[1])), results))


tests = load_legacy_tests_list('legacy_tests.jsonl')


class ExecutionError(Exception):
    pass

def pre_process_func(f):
    shutil.copyfile(f, f+"_backup")
    with open(f, "r") as src:
        sources = src.read()
    with open(f, "w") as src:
        for pragma in add_pragmas:
            src.write("#pragma %s;\n"%pragma)
        src.write(sources)

def post_process_func(f):
    shutil.move(f+"_backup", f)

def compile_func(f):
    res = None
    try:
        pre_process_func(f)
        if "storage-provider.fc" in f :
            # This contract requires building of storage-contract to include it as ref
            with open(f, "r") as src:
                sources = src.read()
                COMPILED_ST_BOC = os.path.join(TMP_DIR, "storage-contract-code.boc")
                sources = sources.replace("storage-contract-code.boc", COMPILED_ST_BOC)
            with open(f, "w") as src:
                src.write(sources)
            COMPILED_ST_FIF = os.path.join(TMP_DIR, "storage-contract.fif")
            COMPILED_ST_BOC = os.path.join(TMP_DIR, "storage-contract-code.boc")
            COMPILED_BUILD_BOC = os.path.join(TMP_DIR, "build-boc.fif")
            res = subprocess.run([FUNC_EXECUTABLE, "-o", COMPILED_ST_FIF, "-SPA", f.replace("storage-provider.fc","storage-contract.fc")], capture_output=False, timeout=10)
            with open(COMPILED_BUILD_BOC, "w") as scr:
                scr.write("\"%s\" include boc>B \"%s\" B>file "%(COMPILED_ST_FIF, COMPILED_ST_BOC))
            res = subprocess.run([FIFT_EXECUTABLE, COMPILED_BUILD_BOC ], capture_output=True, timeout=10)


        res = subprocess.run([FUNC_EXECUTABLE, "-o", COMPILED_FIF, "-SPA", f], capture_output=True, timeout=10)
    except Exception as e:
        post_process_func(f)
        raise e
    else:
        post_process_func(f)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))

def run_runner():
    res = subprocess.run([FIFT_EXECUTABLE, RUNNER_FIF], capture_output=True, timeout=10)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))
    s = str(res.stdout, "utf-8")
    s = s.strip()
    return int(s)

def get_version():
    res = subprocess.run([FUNC_EXECUTABLE, "-s"], capture_output=True, timeout=10)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))
    s = str(res.stdout, "utf-8")
    return s.strip()

success = 0
for ti, (filename_rel, code_hash) in enumerate(tests):
    print("Running test %d/%d: %s" % (ti + 1, len(tests), filename_rel), file=sys.stderr)
    try:
        filename = os.path.join(TESTS_DIR, filename_rel)
        compile_func(filename)
    except ExecutionError as e:
        print(file=sys.stderr)
        print("Compilation error", file=sys.stderr)
        print(e, file=sys.stderr)
        exit(2)

    with open(RUNNER_FIF, "w") as f:
        print("\"%s\" include hash .s" % COMPILED_FIF , file=f)

    try:
        func_out = run_runner()
        if func_out != code_hash:
            raise ExecutionError("Error : expected '%d', found '%d'" % (code_hash, func_out))
        success += 1
    except ExecutionError as e:
        print(e, file=sys.stderr)
        print("Compiled:", file=sys.stderr)
        with open(COMPILED_FIF, "r") as f:
            print(f.read(), file=sys.stderr)
        exit(2)
    print("  OK  ", file=sys.stderr)

print(get_version())
print("Done: Success %d, Error: %d"%(success, len(tests)-success), file=sys.stderr)
