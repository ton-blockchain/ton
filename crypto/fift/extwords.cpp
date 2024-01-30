#include "extwords.h"

#include "Dictionary.h"
#include "IntCtx.h"
#include "SourceLookup.h"

#include "common/refcnt.hpp"
#include "common/util.h"

#include "vm/cellslice.h"
#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/boc.h"

#include "vm/box.hpp"
#include "vm/atom.h"

#include "td/utils/misc.h"
#include "td/utils/PathView.h"

using namespace fift;

void interpret_shell(vm::Stack& stack) {
    auto cmd = stack.pop_string();

    char buffer[32];
    std::vector<char> dynbuff;
    FILE* pipe = popen(cmd.c_str(), "r");

    if (!pipe) throw IntError{"popen() failed while executing"};

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        dynbuff.insert(dynbuff.end(), buffer, buffer + strlen(buffer));
    }

    auto result = std::string(dynbuff.begin(), dynbuff.end());
    pclose(pipe);

    stack.push_string(result);
}

namespace fift {
    void init_words_ext(Dictionary& d) {
        d.def_stack_word("shell ", interpret_shell);
    }
} // fift
