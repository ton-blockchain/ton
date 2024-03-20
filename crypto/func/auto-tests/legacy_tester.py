import os
import os.path
import subprocess
import sys
import tempfile
import shutil

add_pragmas = []

tests = [
    # note, that deployed version of elector,config and multisig differ since it is compilled with func-0.1.0.
    # Newer compillators optimize arithmetic and logic expression that can be calculated at the compile time
    # FunC v0.5.0 enabled compute-asm-ltd and allow-post-modifications by default, this changed almost all compilations

    ["elector/elector-code.fc", 96915288474895095375636985346314354662782784860648058159878921867365312683154],
    ["config/config-code.fc", 22290399841823616942485152190379024190655691555189464909245612965253206016940],
    ["eth-bridge-multisig/multisig-code.fc", 61794576422183943960272839720117943378686303983578297509099019523440847170337],

    ["bsc-bridge-collector/votes-collector.fc", 92944270384900724656771477263840139047476985690583064733832286422277180224175],
    ["uni-lock-wallet/uni-lockup-wallet.fc", 27323712737699566411771249412638860892056222617959925563185009479925143218351],
    ["nft-collection/nft-collection-editable.fc", 3265272879406938912616104900469474227258611213495548733729957432831667074606],
    ["dns-collection/nft-collection.fc", 50100285311144683401305558797522852568981620193836838830789475785944491291928],


    # note, that deployed version of tele-nft-item differs since it is compilled with func-0.3.0.
    # After introducing of try/catch construction, c2 register is not always the default one.
    # Thus it is necessary to save it upon jumps, differences of deployed and below compilled is that
    # "c2 SAVE" is added to the beginning of recv_internal. It does not change behavior.
    ["tele-nft-item/nft-item.fc", 112456603551352598193405120624678866030139400186800709562240012518003340977105],

    ["storage/storage-contract.fc", 44542652015163304335966522853331133393011733370692441537470366854345658892851],
    ["storage/storage-provider.fc", 108677173951298337060746154977967122806502520160062519672276937694037317935577],
    ["nominator-pool/pool.fc", 113824867250406571749406540634759859835643042958487527298742314026185451318138],
    ["jetton-minter/jetton-minter.fc", 59172251235013928378816323931018560572240088017859486196396002876800156186183],
    ["gg-marketplace/nft-marketplace-v2.fc", 65550944551194537105854154716861234168502062117999272754502885031166440057836],
    ["jetton-wallet/jetton-wallet.fc", 26109413028643307141901410795152471606217725316052170190064118584402007124948],

    # 2023-11-30 update: source files in nominators.fc were included in the wrong order.
    # FunC v0.5.0 has more strict function ordering rules, so nominators.fc was changed to fix compilation error.
    ["whales-nominators/nominators.fc", 32017040532734767645954674692768344406402364921654403435168703583553605058036],


    # 2023-11-30 update: __tact_verify_address was optimized out because it is not marked as impure.
    # In FunC v0.5.0 all functions are impure by default, so it is not optimized out anymore.
    ["tact-examples/treasure_Treasure.code.fc", 60348994682690664630455354672413353326734169633005380820521037982881341539662],
    ["tact-examples/jetton_SampleJetton.code.fc", 70075170397574779402104489003369966746142305378310979462742589461276337980270],
    ["tact-examples/jetton_JettonDefaultWallet.code.fc", 12710666798574325848563501630728896968396634924087634181325633274893128358499],
    ["tact-examples/maps_MapTestContract.code.fc", 86004071444084135394990759334664637743689750108042267324839281046456545168857],
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
        print("Compiled:", file=sys.stderr)
        with open(COMPILED_FIF, "r") as f:
            print(f.read(), file=sys.stderr)
        exit(2)
    print("  OK  ", file=sys.stderr)

print(get_version())
print("Done: Success %d, Error: %d"%(success, len(tests)-success), file=sys.stderr)