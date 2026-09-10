"""Shared PromQL fragments for the node-local external-message pool."""

from .conventions import histogram_quantile, summed
from .core import NODE, sel

METRIC = "ton_mempool_ext_messages"
STATES = "~eligible|postponed"


def _gauge(state):
    return summed("job, instance", f"{METRIC}{sel(**NODE, state=state)}")


def eligible():
    """Messages eligible for collation, with the legacy unsplit gauge as history fallback."""
    return f"({_gauge('eligible')}) or ({_gauge('')})"


def total():
    """All stored messages; sum the state buckets, or use the legacy unsplit gauge."""
    return f"({_gauge(STATES)}) or ({_gauge('')})"


def _stateful_nodes():
    """Presence marker for the schema where accepted excludes validate-only successes."""
    return f"max by (job, instance) ({METRIC}{sel(**NODE, state='eligible')}) >= 0"


def _only_stateful(inner):
    return f"({inner}) and on (job, instance) ({_stateful_nodes()})"


def expiry_handler_lag():
    """Seconds the oldest stored entry is past its 600-second expiry deadline."""
    oldest = f"ton_mempool_oldest_ext_message_age_seconds{sel(**NODE)}"
    inner = summed("job, instance", f"clamp_min({oldest} - 600, 0)", agg="max")
    return _only_stateful(inner)


def oldest_age():
    """Oldest stored-entry age, omitting legacy nodes whose expiry semantics differ."""
    inner = summed(
        "job, instance",
        f"ton_mempool_oldest_ext_message_age_seconds{sel(**NODE)}",
        agg="max",
    )
    return _only_stateful(inner)


def inclusion_quantile(q):
    """One node's quantile of how long an entry it stored waited until that node saw it applied.

    Fixed five minutes, like every other event quantile on the boards: inclusions arrive in
    block-sized bursts that a scrape-sized window would turn into a sawtooth.
    """
    return histogram_quantile(q, "ton_mempool_ext_inclusion_seconds", by="job, instance",
                              over="5m", **NODE)


def stock_delta(over="5m"):
    """Endpoint-matched change in total stateful storage over the window."""
    body = f"delta({METRIC}{sel(**NODE, state=STATES)}[{over}])"
    return summed("job, instance", body)


def accounted_delta(over="5m"):
    """New insertions minus every removal over the window; reprioritization changes neither."""
    inserted = summed(
        "job, instance",
        f"increase(ton_mempool_ext_admission_total{sel(**NODE, outcome='accepted')}[{over}])",
    )
    removed = summed(
        "job, instance",
        f"increase(ton_mempool_ext_removed_total{sel(**NODE)}[{over}])",
    )
    return _only_stateful(f"({inserted}) - ({removed})")


def unexplained_delta(over="5m"):
    """Magnitude of stock delta not reconciled by insertion and removal counters."""
    return f"abs(({stock_delta(over)}) - ({accounted_delta(over)}))"
