import contextlib
from typing import final, override

import gdb  # pyright: ignore[reportMissingModuleSource]

MESSAGE_RUN_HOOK = "td::actor::core::gdb::hook_message_run"
MESSAGE_PUSHED_HOOK = "td::actor::core::gdb::hook_message_pushed_to_mailbox"
MESSAGE_DELAYED_HOOK = "td::actor::core::gdb::hook_message_delayed"

FLUSH_FN = "td::actor::core::ActorExecutor::flush_one_message"


def has_flush_frame():
    f = gdb.newest_frame()
    while f:
        if f.name() == FLUSH_FN:
            return True
        f = f.older()
    return False


@final
class MallocReturnBP(gdb.Breakpoint):
    def __init__(self, addr: int):
        super().__init__("malloc", internal=True)
        self.condition = f"$_retval == (void*){addr}"
        self.silent = True


def reverse_continue_until_any(breakpoints: list[gdb.Breakpoint]):
    while True:
        _ = gdb.execute("reverse-continue", to_string=True)
        if any(bp.hit_count > 0 for bp in breakpoints):
            return


@contextlib.contextmanager
def with_breakpoint(bp: gdb.Breakpoint):
    try:
        yield bp
    finally:
        bp.delete()


def run():
    if not has_flush_frame():
        print("flush_one_message not present in the backtrace")
        return

    thread_name = gdb.selected_thread().num

    with with_breakpoint(gdb.Breakpoint(MESSAGE_RUN_HOOK, internal=True)) as bp:
        bp.silent = True
        bp.thread = thread_name
        reverse_continue_until_any([bp])

    mailbox = gdb.parse_and_eval("&mailbox")
    message = gdb.parse_and_eval("message.impl_.get()")

    with (
        with_breakpoint(gdb.Breakpoint(MESSAGE_PUSHED_HOOK, internal=True)) as bp_pushed,
        with_breakpoint(gdb.Breakpoint(MESSAGE_DELAYED_HOOK, internal=True)) as bp_delayed,
    ):
        bp_pushed.silent = True
        bp_pushed.condition = f"&mailbox == {mailbox} && message.impl_.get() == {message}"

        bp_delayed.silent = True
        bp_delayed.condition = f"&mailbox == {mailbox} && message.impl_.get() == {message}"

        reverse_continue_until_any([bp_pushed, bp_delayed])


class ActorTraceCmd(gdb.Command):
    def __init__(self):
        super().__init__("reverse-continue-to-actor-caller", gdb.COMMAND_USER)

    @override
    def invoke(self, argument: str, from_tty: bool):
        run()


_ = ActorTraceCmd()
_ = gdb.execute("alias rc-actor = reverse-continue-to-actor-caller", to_string=True)
_ = gdb.execute("alias rca = reverse-continue-to-actor-caller", to_string=True)
