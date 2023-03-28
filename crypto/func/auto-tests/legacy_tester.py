import os
import os.path
import subprocess
import sys
import tempfile
import shutil

add_pragmas = [] #["allow-post-modification", "compute-asm-ltr"];

tests = [
  # note, that deployed version of elector,config and multisig differ since it is compilled with func-0.1.0.
  # Newer compillators optimize arithmetic and logic expression that can be calculated at the compile time
  ["elector/elector-code.fc", 115226404411715505328583639896096915745686314074575650766750648324043316883483],
  ["config/config-code.fc", 10913070768607625342121305745084703121685937915388357634624451844356456145601],
  ["eth-bridge-multisig/multisig-code.fc", 101509909129354488841890823627011033360100627957439967918234053299675481277954],

  ["bsc-bridge-collector/votes-collector.fc", 62190447221288642706570413295807615918589884489514159926097051017036969900417],
  ["uni-lock-wallet/uni-lockup-wallet.fc", 61959738324779104851267145467044677651344601417998258530238254441977103654381],
  ["nft-collection/nft-collection-editable.fc", 45561997735512210616567774035540357815786262097548276229169737015839077731274],
  ["dns-collection/nft-collection.fc", 107999822699841936063083742021519765435859194241091312445235370766165379261859],


  # note, that deployed version of tele-nft-item differs since it is compilled with func-0.3.0.
  # After introducing of try/catch construction, c2 register is not always the default one.
  # Thus it is necessary to save it upon jumps, differences of deployed and below compilled is that
  # "c2 SAVE" is added to the beginning of recv_internal. It does not change behavior.
  ["tele-nft-item/nft-item.fc", 69777543125381987786450436977742010705076866061362104025338034583422166453344],

  ["storage/storage-contract.fc", 91377830060355733016937375216020277778264560226873154627574229667513068328151],
  ["storage/storage-provider.fc", 13618336676213331164384407184540461509022654507176709588621016553953760588122],
  ["nominator-pool/pool.fc", 69767057279163099864792356875696330339149706521019810113334238732928422055375],
  ["jetton-minter/jetton-minter.fc", 9028309926287301331466371999814928201427184114165428257502393474125007156494],
  ["gg-marketplace/nft-marketplace-v2.fc", 92199806964112524639740773542356508485601908152150843819273107618799016205930],
  ["jetton-wallet/jetton-wallet.fc", 86251125787443633057458168028617933212663498001665054651523310772884328206542],
  ["whales-nominators/nominators.fc", 8941364499854379927692172316865293429893094891593442801401542636695127885153],


  ["tact-examples/treasure_Treasure.code.fc", 13962538639825790677138656603323869918938565499584297120566680287245364723897],
  ["tact-examples/jetton_SampleJetton.code.fc", 94076762218493729104783735200107713211245710256802265203823917715299139499110],
  ["tact-examples/jetton_JettonDefaultWallet.code.fc", 29421313492520031238091587108198906058157443241743283101866538036369069620563],
   ["tact-examples/maps_MapTestContract.code.fc", 22556550222249123835909180266811414538971143565993192846012583552876721649744], 
]

def getenv(name, default=None):
    if name in os.environ:
        return os.environ[name]
    if default is None:
        print("Environment variable", name, "is not set", file=sys.stderr)
        exit(1)
    return default

FUNC_EXECUTABLE = getenv("FUNC_EXECUTABLE", "func")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
FIFT_LIBS = getenv("FIFTPATH")
TMP_DIR = tempfile.mkdtemp()

COMPILED_FIF = os.path.join(TMP_DIR, "compiled.fif")
RUNNER_FIF = os.path.join(TMP_DIR, "runner.fif")

TESTS_DIR = "legacy_tests"

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
    res = subprocess.run([FIFT_EXECUTABLE, "-I", FIFT_LIBS, RUNNER_FIF], capture_output=True, timeout=10)
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
for ti, t in enumerate(tests):
    tf, th = t
    print("  Running test %d/%d: %s" % (ti + 1, len(tests), tf), file=sys.stderr)
    tf = os.path.join(TESTS_DIR, tf)
    try:
        compile_func(tf)
    except ExecutionError as e:
        print(file=sys.stderr)
        print("Compilation error", file=sys.stderr)
        print(e, file=sys.stderr)
        exit(2)

    with open(RUNNER_FIF, "w") as f:
        print("\"%s\" include hash .s" % COMPILED_FIF , file=f)

    try:
        func_out = run_runner()
        if func_out != th:
                raise ExecutionError("Error : expected '%d', found '%d'" % (th, func_out))
        success += 1
    except ExecutionError as e:
        print(e, file=sys.stderr)
        #print("Compiled:", file=sys.stderr)
        #with open(COMPILED_FIF, "r") as f:
        #    print(f.read(), file=sys.stderr)
        #exit(2)
    print("  OK  ", file=sys.stderr)

print(get_version())
print("Done: Success %d, Error: %d"%(success, len(tests)-success), file=sys.stderr)

