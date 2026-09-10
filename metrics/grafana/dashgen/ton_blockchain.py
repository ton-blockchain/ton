"""TON Blockchain: chain state, where block-production time goes, and external messages."""

from .lib import (
    FLEET_SPREAD_STYLES,
    JOB,
    JOB_SEL,
    NODE,
    SEL,
    SLOT,
    agg_line,
    agg_timeseries,
    agg_variable,
    attributed,
    chain_timeseries,
    dashboard,
    failure_bands,
    fleet_spread,
    fleet_timeseries,
    gauge,
    histogram_quantile,
    line,
    mempool,
    nav_link,
    plain_stat,
    rate,
    ratio,
    reference_line,
    row,
    same_reporters,
    sel,
    series_style,
    slot_variable,
    stacked_timeseries,
    standard_variables,
    summary_column,
    summary_table,
    summed,
    table_legend,
    threshold_lines,
    thresholds,
    unstacked_total,
    variable_custom,
    worst_node_stat,
)

SELF = "ton-blockchain"
RATE = "$__rate_interval"
# $chain expands to "master|-1" or "shard|0", so one value filters both chain= and workchain=.
CHAIN = "~${chain:raw}"

# The collator's outer phases in the order it runs them (timer scopes in collator.cpp). Keep this
# single list as the selector and stable legend order for mean and retained-outlier panels. Nested
# transaction/preliminary-storage timers are deliberately separate so nobody adds them twice.
PIPELINE_ORDER = ("preinit", "queue_cleanup", "dispatch_queue", "import_internals",
                  "import_externals", "process_new_msgs", "enqueue_new_messages",
                  "final_storage_stat", "combine_account_transactions", "create_shard_state",
                  "create_block", "create_collated_data", "create_block_candidate")
PIPELINE = "~" + "|".join(PIPELINE_ORDER)
PIPELINE_LABELS = (
    ("preinit", "Pre-init"),
    ("queue_cleanup", "Queue cleanup"),
    ("dispatch_queue", "Dispatch queue"),
    ("import_internals", "Import internals"),
    ("import_externals", "Import externals"),
    ("process_new_msgs", "Process new messages"),
    ("enqueue_new_messages", "Enqueue new messages"),
    ("final_storage_stat", "Final storage stats"),
    ("combine_account_transactions", "Combine transactions"),
    ("create_shard_state", "Create shard state"),
    ("create_block", "Create block"),
    ("create_collated_data", "Create collated data"),
    ("create_block_candidate", "Create candidate"),
)

# The validator's phases in the order validate-query runs them (validate-query.cpp), the four
# transaction timers where the per-transaction checking happens. Validation work can run in
# parallel contexts, so these are drawn as lines and never stacked.
VALIDATION_ORDER = ("unpack_block_candidate", "process_mc_state", "validate_block_tlb",
                    "unpack_state", "unpack_block_data", "precheck_account_updates",
                    "precheck_account_transactions", "precheck_msg_queue",
                    "unpack_dispatch_queue", "check_in_msg_descr", "check_out_msg_descr",
                    "check_dispatch_queue", "check_processed_upto", "check_in_queue",
                    "trx_tvm", "trx_storage_stat", "trx_other", "check_transactions_other",
                    "check_new_state")
VALIDATION = "~" + "|".join(VALIDATION_ORDER)
TRANSACTIONS = "~trx_tvm|trx_storage_stat|trx_other"
REJECTED = "!~accepted|validated_only|duplicate|reprioritized"
STORAGE_OPS = ("~check_signature_consensus|apply_block_to_state(_fast)?"
               "|serialize_state(_to_file)?|transaction_storage_stat_[ab]"
               "|cell_(load|store|store_refcnt_diff)"
               "|celldb_(store_cell|store_cell_multi|prepare_stats|gc_cell)"
               "|rocksdb_commit_(write_batch|transaction)|fd_sync")


def blocks(kind="collated", *, over=RATE, node=False, **labels):
    """Blocks collated or validated; chain truth uses every reporter unless `node` is set."""
    reporters = NODE if node else JOB
    return f"rate(ton_{kind}_blocks_total{sel(**reporters, chain=CHAIN, **labels)}[{over}])"


def per_block(numerator, *, by=None, kind="collated", over=RATE, group_left=False):
    """A rate divided by the blocks it went into; where nothing was produced, no series.

    `group_left` joins on `by` rather than matching label sets, for a numerator carrying labels
    (a phase, say) that the block count does not.
    """
    made = summed(by, blocks(kind, over=over, node=bool(by and "instance" in by), result="ok"))
    join = f" on ({by}) group_left ()" if group_left else ""
    return f"{numerator} /{join} ({made} > 0)"


def share_of_block(numerator, *, kind="collated"):
    """Per-block share for a stacked breakdown: a scalar divisor keeps the phase label."""
    made = summed(None, blocks(kind, result="ok"))
    return f"{numerator} / scalar(clamp_min({made}, 1e-9)) and on () ({made} > 0)"


def phase_seconds(operation, *, node=False, **labels):
    """The block-processing timer for one operation and phase of the selected chain."""
    scope = sel(**(NODE if node else JOB), operation=operation, chain=CHAIN, **labels)
    return f"ton_block_processing_seconds_total{scope}"


def stage(operation, *, node=False, **labels):
    """Seconds one phase of collating or validating spends per second, chain-wide."""
    return f"rate({phase_seconds(operation, node=node, **labels)}[{RATE}])"


def collated_seconds(clock, phase):
    """Chain-wide collation seconds for one clock and phase set."""
    return summed(None, stage("collate", clock=clock, result="ok", phase=phase))


def phase_per_node(operation, phases, kind):
    """Real and CPU seconds one node spends in each phase, per block it processed itself.

    The chain-scope phase panels answer "where does a block's time go"; this is the same
    decomposition kept per node, which is what answers "and on which node".
    """
    spent = summed("job, instance, clock, phase",
                   stage(operation, node=True, clock="~real|cpu", result="ok", phase=phases))
    return per_block(spent, by="job, instance", kind=kind, group_left=True)


def processing_quantile(operation, clock, q):
    """A fixed-five-minute event quantile for successful whole-block processing."""
    return histogram_quantile(
        q,
        "ton_block_processing_duration_seconds",
        over="5m",
        **JOB,
        operation=operation,
        chain=CHAIN,
        result="ok",
        clock=clock,
    )


def processing_quantiles(operation, q, *, chain=CHAIN):
    """A fixed-five-minute event quantile, returned as one row per clock."""
    return histogram_quantile(
        q,
        "ton_block_processing_duration_seconds",
        by="clock",
        over="5m",
        **JOB,
        operation=operation,
        chain=chain,
        result="ok",
        clock="~elapsed|real|cpu",
    )


def processing_recent_max(operation, clock, result):
    """Each node's exact retained maximum whole-block observation."""
    return gauge(
        "ton_block_processing_duration_recent_max_seconds",
        "job, instance",
        agg="max",
        operation=operation,
        chain=CHAIN,
        result=result,
        clock=clock,
    )


def processing_recent_max_fleet(operation, *, chain=CHAIN):
    """Exact recent successful maxima, retaining the owning node and clock.

    This intentionally ignores the Instance selector: the collator rotates, and the summary must
    not lose the event owner merely because a different node happens to be selected for drilldown.
    """
    metric = "ton_block_processing_duration_recent_max_seconds"
    scope = sel(**JOB, operation=operation, chain=chain, result="ok",
                clock="~elapsed|real|cpu")
    return f"topk by (clock) (1, max by (clock, job, instance) ({metric}{scope}))"


def phase_recent_max(operation, phases):
    """Each node's retained maximum for every result/real-or-CPU processing phase."""
    return gauge(
        "ton_block_processing_phase_recent_max_seconds",
        "result, clock, phase, job, instance",
        agg="max",
        operation=operation,
        chain=CHAIN,
        result="~ok|error",
        phase=phases,
        clock="~real|cpu",
    )


def processing_cpu_load(operation):
    """CPU seconds per wall second, including successful and failed attempts."""
    return summed(
        "job, instance",
        stage(operation, node=True, phase="total", clock="cpu"),
    )


def collation_phase_mean(clock, phase):
    """Seconds in one outer collation phase per successful block, chain-wide."""
    return per_block(collated_seconds(clock, phase))


def collation_other_work(clock):
    """Measured active work not covered by the additive outer phase timers."""
    total = collated_seconds(clock, "total")
    outer = collated_seconds(clock, PIPELINE)
    return per_block(f"clamp_min(({total}) - ({outer}), 0)")


def collation_wait(phase):
    """One elapsed wait category per successful block."""
    return per_block(collated_seconds("elapsed", phase))


def collation_other_wait():
    """End-to-end time that is neither active real work nor the explicit external wait."""
    elapsed = collated_seconds("elapsed", "total")
    real = collated_seconds("real", "total")
    externals = collated_seconds("elapsed", "wait_externals")
    return per_block(f"clamp_min(({elapsed}) - ({real}) - ({externals}), 0)")


def first_window_per_block(metric, first):
    """Seconds or work per successfully collated block, by leader-window position."""
    made = summed(None, blocks(result="ok", first=first))
    spent = summed(None, f"rate({metric}{sel(**JOB, chain=CHAIN, first=first)}[{RATE}])")
    return f"{spent} / ({made} > 0)"


def queue_progress(metric):
    """Out-msg queue messages handled per successfully collated block."""
    return per_block(summed(None, collation_rate(metric)))


def error_share(kind, by=None):
    """Failed attempts over all attempts of one production stage."""
    node = bool(by and "instance" in by)
    return ratio(summed(by, blocks(kind, node=node, result="error")),
                 summed(by, blocks(kind, node=node)))


def error_contribution(kind):
    """Each node's slice of the chain-wide error share; the slices sum to the drawn line.

    Errors are sparse, so a range-dominant owner explains nothing at the moment of a spike: the
    range winner is usually clean right then, and the row would truthfully show 0 next to a
    nonzero drawn value. Contributions instead name exactly the nodes erring now, and their sum
    is the drawn value by construction (per-node numerator over the same chain-wide denominator).
    """
    return (f'{summed("job, instance", blocks(kind, result="error"))}'
            f' / on () group_left () ({summed(None, blocks(kind))} > 0) > 0')


def rejections():
    """Admissions the node refused, excluding every successful or stock-neutral outcome."""
    return f"rate(ton_mempool_ext_admission_total{sel(**NODE, outcome=REJECTED)}[{RATE}])"


def removals():
    """Externals removed from one node's mempool, by reason."""
    removed = rate("ton_mempool_ext_removed_total", "reason, job, instance", over="5m")
    return f"{removed} > 0"


def collation_rate(metric):
    """A per-collation counter for the selected chain."""
    return f"rate({metric}{sel(**JOB, chain=CHAIN)}[{RATE}])"


def by_source(metric):
    """Block arrivals per source, as a share of the blocks the node actually applied."""
    def arrivals(name, by):
        return summed(by, f"rate({name}{sel(**NODE, workchain=CHAIN)}[{RATE}])")

    applied = arrivals("ton_first_received_total", "job, instance")
    return summed("source", f"{arrivals(metric, 'source, job, instance')}"
                            f" / on (job, instance) group_left() ({applied} > 0)", agg="avg")


def masterchain_rate(over):
    """Masterchain blocks/s: the seqno delta when every node reports arrivals, else their median."""
    seqno = f"ton_masterchain_seqno{JOB_SEL}"
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='-1')}"
    delta = f"clamp_min((max({seqno}) - max({seqno} offset 1m)) / 60, 0)"
    everyone_reports = same_reporters(seqno, arrivals)
    median = f"quantile(0.5, sum by (job, instance) (rate({arrivals}[{over}])))"
    return f"({delta} and on () ({everyone_reports})) or {median}"


def shard_rate(over):
    """Shardchain blocks/s as the median node's first-arrival rate."""
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='0')}"
    return f"quantile(0.5, sum by (job, instance) (rate({arrivals}[{over}])))"


def shard_collations(over):
    """Shard blocks this fleet collated successfully - the denominator of the two shard tiles."""
    return summed(None, f"rate(ton_collated_blocks_total"
                        f"{sel(**JOB, chain='shard', result='ok')}[{over}])")


def collation_budget(over):
    """Collation seconds per produced block, minus waiting, as a share of the configured slot."""
    def phase(name):
        spent = sel(**JOB, operation="collate", chain="shard", phase=name,
                    clock="elapsed", result="ok")
        return summed(None, f"rate(ton_block_processing_seconds_total{spent}[{over}])")

    return (f"({phase('total')} - {phase('wait_externals')})"
            f" / ({shard_collations(over)} > 0) / {SLOT}")


def discarded_share(over):
    """Candidates collated but never applied - trustworthy only while every node reports both."""
    counted = f"ton_collated_blocks_total{sel(**JOB, chain='shard', result='ok')}"
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='0')}"
    both_report = same_reporters(counted, arrivals)
    return (f"clamp_min(1 - {shard_rate(over)} / {shard_collations(over)}, 0)"
            f" and on () ({shard_collations(over)} > 0) and on () ({both_report})")


def synced_applied(over):
    """Applied externals per second, counting only nodes whose view of the chain is current."""
    fresh = f"abs(ton_masterchain_block_age_seconds{JOB_SEL}) < 120"
    caught_up = (f"clamp_min(ton_masterchain_seqno{JOB_SEL}"
                 f" - ton_shardclient_seqno{JOB_SEL}, 0) <= 2")

    def applied(chain):
        # the literal pins which guard applies; $chain empties the branch of the other one
        return f"rate(ton_applied_ext_messages_total{sel(**JOB, chain=[chain, CHAIN])}[{over}])"

    master = f"({applied('master')} and on (job, instance) ({fresh}))"
    shard = (f"({applied('shard')} and on (job, instance) ({fresh})"
             f" and on (job, instance) ({caught_up}))")
    return f"quantile(0.5, {master} or {shard})"


def accepted(over):
    """Successfully validated externals, averaged per node so fleet size does not inflate it."""
    admissions = sel(**NODE, outcome="~accepted|reprioritized|validated_only")
    return summed(None, summed("job, instance",
                               f"rate(ton_mempool_ext_admission_total{admissions}[{over}])"),
                  agg="avg")


def collated_externals(over=RATE, **labels):
    """Externals a collation attempt offered, included or rejected."""
    return f"rate(ton_collation_ext_messages_total{sel(**JOB, chain=CHAIN, **labels)}[{over}])"


def stage_node_observations(suffix):
    """Fixed-five-minute consensus-stage observations kept per reporter."""
    metric = f"ton_consensus_stage_seconds_{suffix}{sel(**JOB, chain=CHAIN)}"
    return summed("stage, job, instance", f"rate({metric}[5m])")


def stage_node_mean():
    """Each node's five-minute mean for every stage it actually observed."""
    return ratio(stage_node_observations("sum"), stage_node_observations("count"))


def owner_labeled(expr, owner):
    """Copy ``instance`` to a display label, then drop it so summary frames can merge."""
    labeled = f'label_replace(({expr}), "{owner}", "$1", "instance", "(.*)")'
    return f"max without (instance) ({labeled})"


def slowest_stage_mean():
    """Highest five-minute per-node mean for each stage, retaining that node as context."""
    return owner_labeled(f"topk by (stage) (1, {stage_node_mean()})", "slow_owner")


def slowest_stage_samples():
    """Number of observations behind each slowest-node mean."""
    metric = f"ton_consensus_stage_seconds_count{sel(**JOB, chain=CHAIN)}"
    count = summed("stage, job, instance", f"increase({metric}[5m])")
    slowest = f"topk by (stage) (1, {stage_node_mean()})"
    selected = f"({count}) and on (stage, job, instance) ({slowest})"
    return owner_labeled(selected, "slow_owner")


def stage_quantile(q):
    """A fixed-five-minute quantile for every observed consensus stage."""
    return histogram_quantile(
        q,
        "ton_consensus_stage_seconds",
        by="stage",
        over="5m",
        **JOB,
        chain=CHAIN,
    )


def stage_recent_max_summary():
    """Fleet exact recent maximum for each stage, retaining its node as table context."""
    metric = "ton_consensus_stage_recent_max_seconds"
    per_node = f"max by (stage, job, instance) ({metric}{sel(**JOB, chain=CHAIN)})"
    return owner_labeled(f"topk by (stage) (1, {per_node})", "max_owner")


def slot_mark_quantile(side, q, mark=None, *, chain=CHAIN):
    """Fixed-five-minute quantile of one zero-padded side of slot zero."""
    labels = {**JOB, "chain": chain, "side": side}
    if mark is not None:
        labels["mark"] = mark
    return histogram_quantile(
        q,
        "ton_consensus_collator_slot_mark_seconds",
        by="mark",
        over="5m",
        **labels,
    )


def slot_mark_median(mark=None, *, chain=CHAIN):
    """Signed median from paired sides with identical populations and an exact zero bucket."""
    after = slot_mark_quantile("after", 0.5, mark, chain=chain)
    before = slot_mark_quantile("before", 0.5, mark, chain=chain)
    return f"({after}) - ({before})"


def slot_mark_recent_max_summary(side, marks):
    """Latest retained position for selected marks, keeping the responsible collator."""
    metric = "ton_consensus_collator_slot_mark_recent_max_seconds"
    scope = sel(**JOB, chain=CHAIN, side=side, mark=marks)
    per_node = f"max by (mark, job, instance) ({metric}{scope})"
    return owner_labeled(f"topk by (mark) (1, {per_node})", "max_owner")


def handoff_quantile(side, q):
    """A fixed-five-minute quantile of one side of the handoff, chain-wide."""
    return histogram_quantile(q, f"ton_consensus_{side}_seconds", over="5m",
                              **JOB, chain=CHAIN)


def category_labeled(expr, label, value):
    """Give an otherwise scalar vector a categorical label for a summary-table row."""
    return f'label_replace(({expr}), "{label}", "{value}", "", "")'


def handoff_quantile_summary(q):
    """Dead-time and head-start quantiles as two rows of one table."""
    dead = category_labeled(handoff_quantile("slot_gap", q), "side", "dead_time")
    lead = category_labeled(handoff_quantile("slot_lead", q), "side", "head_start")
    return f"{dead} or {lead}"


def handoff_recent_max_summary():
    """Exact retained maximum for each handoff side, with collator ownership."""
    def one(metric_suffix, side):
        metric = f"ton_consensus_{metric_suffix}_recent_max_seconds"
        per_node = f"max by (job, instance) ({metric}{sel(**JOB, chain=CHAIN)})"
        worst = f"topk(1, {per_node})"
        with_side = category_labeled(worst, "side", side)
        return owner_labeled(with_side, "max_owner")

    return f"{one('slot_gap', 'dead_time')} or {one('slot_lead', 'head_start')}"


def below_zero(expr):
    """The same magnitude drawn below its panel's reference event."""
    return f"-1 * ({expr})"


def round_outcome_rate():
    """Five-minute terminal-round rate per validator group, averaged across validators.

    Rounds are reported by validator identities only, so the numerator and ton_validator_groups
    count the same population however many identities a node runs for one group.
    """
    rounds = f"rate(ton_consensus_rounds_total{sel(**JOB, chain=CHAIN)}[5m])"
    observed = summed("outcome, job, instance", rounds)
    groups = f"avg_over_time(ton_validator_groups{sel(**JOB, chain=CHAIN)}[5m])"
    group_count = summed("job, instance", groups, agg="max")
    per_group = f"{observed} / on (job, instance) group_left () ({group_count} > 0)"
    return summed("outcome", per_group, agg="avg")


def round_terminal_rate():
    """Certified terminal slots per second for one average validator group."""
    return summed(None, round_outcome_rate())


def round_outcome_share():
    """How the certified terminal slots split between block, empty, and skipped outcomes."""
    outcomes = round_outcome_rate()
    terminal = summed(None, outcomes)
    return f"{outcomes} / on () group_left () ({terminal} > 0)"


SPREAD_STYLES = FLEET_SPREAD_STYLES
HISTORY_LEGEND = table_legend("lastNotNull", "min", "max")
SOURCE_LEGEND = table_legend("lastNotNull", "mean", "max", sort="Mean", placement="right")

MASTERCHAIN_AGE = f"clamp_min(ton_masterchain_block_age_seconds{SEL}, 0)"
SHARD_LAG = f"clamp_min(ton_masterchain_seqno{SEL} - ton_shardclient_seqno{SEL}, 0)"

VARIABLES = [
    *standard_variables(),
    agg_variable(),
    slot_variable(),
    variable_custom(
        "chain", label="Chain", choices=[("Masterchain", "master|-1"), ("Shardchain", "shard|0")],
        selected="Shardchain",
        description=(
            "Select exactly one chain family. The value maps masterchain to "
            "chain=master/workchain=-1 and shardchain to chain=shard/workchain=0 so the two never "
            "share a graph."
        ),
    ),
]


def production_diagnostic_rows(panels):
    """Partition production detail by operator question, with no mixed grab-bag row."""
    specs = (
        ("Collation diagnostics", 102, (8, 47, 49, 51, 53, 70)),
        ("Validation diagnostics", 108, (56, 48, 50, 52, 54)),
        ("Block workload & storage", 109, (37, 23, 24, 38, 68, 69)),
    )
    by_id = {panel["id"]: panel for panel in panels}
    expected = {panel_id for _, _, ids in specs for panel_id in ids}
    if set(by_id) != expected:
        raise ValueError("production diagnostic panel registry is incomplete")
    return [
        row(title, id=row_id, collapsed=True, panels=[by_id[panel_id] for panel_id in ids])
        for title, row_id, ids in specs
    ]


def external_message_rows(panels):
    """Separate end-to-end message flow from node-local mempool health."""
    specs = (
        ("External message flow", 103, (25, 26, 29, 10, 11)),
        ("Mempool diagnostics", 110, (27, 28, 30, 12, 71, 72)),
    )
    by_id = {panel["id"]: panel for panel in panels}
    expected = {panel_id for _, _, ids in specs for panel_id in ids}
    if set(by_id) != expected:
        raise ValueError("external-message panel registry is incomplete")
    return [
        row(title, id=row_id, collapsed=True, panels=[by_id[panel_id] for panel_id in ids])
        for title, row_id, ids in specs
    ]


# The at-a-glance row shows both chains side by side, so its summary tables and slot timelines pin a
# chain literal instead of reading $chain. One builder per pair; the two collate stacks between them
# read $chain like everything below the row.
PINNED_NOTE = ("deliberately ignores the Chain selector, so both chains stay visible at a glance; "
          "the summary tables and slot timelines of this row are chain-pinned pairs, while the two "
          "collate stacks between them follow Chain, as does every collapsed diagnostic row. ")


def collation_glance(chain, chain_name, *, id):
    """One chain's compact collation-latency table."""
    return summary_table(
        f"Collation summary (5m) — {chain_name}",
        "clock",
        [
            summary_column("p50", processing_quantiles("collate", 0.5, chain=chain), unit="s"),
            summary_column("p95", processing_quantiles("collate", 0.95, chain=chain), unit="s"),
            summary_column("recent max",
                           processing_recent_max_fleet("collate", chain=chain), unit="s"),
        ],
        id=id, w=12, h=7, key_name="Clock",
        key_mappings=(("elapsed", "End-to-end"), ("real", "Timed wall"), ("cpu", "CPU")),
        label_columns=(("instance", "max owner"),), query_scope="job",
        description=(
            f"Successful collation latency for the {chain_name.lower()} in one compact view. "
            f"This panel is pinned to chain={chain} and " + PINNED_NOTE +
            "p50 and p95 are event quantiles pooled across all rotating collators over a fixed "
            "five-minute window; they are not quantiles of node averages. Recent max is an exact "
            "per-attempt maximum retained for roughly 10–20 minutes, and max owner names the "
            "responsible node. End-to-end includes all pacing and waits, timed wall is measured "
            "active wall time, and CPU is process execution time. The population is job-wide "
            "because collators rotate, so this intentionally ignores Instance."
        ),
    )


def validation_glance(chain, chain_name, *, id):
    """One chain's compact validation-latency table, in the same shape as collation."""
    return summary_table(
        f"Validation summary (5m) — {chain_name}",
        "clock",
        [
            summary_column("p50", processing_quantiles("validate", 0.5, chain=chain), unit="s"),
            summary_column("p95", processing_quantiles("validate", 0.95, chain=chain), unit="s"),
            summary_column("recent max",
                           processing_recent_max_fleet("validate", chain=chain), unit="s"),
        ],
        id=id, w=12, h=7, key_name="Clock",
        key_mappings=(("elapsed", "End-to-end"), ("real", "Timed wall"), ("cpu", "CPU")),
        label_columns=(("instance", "max owner"),), query_scope="job",
        description=(
            f"Successful validation latency for the {chain_name.lower()}, in the same shape as "
            f"collation. This panel is pinned to chain={chain} and " + PINNED_NOTE +
            "p50 and p95 are event quantiles pooled across the validators of the selected job "
            "over a fixed five-minute window. Recent max is an exact retained observation and max "
            "owner names its validator. Compare end-to-end, timed wall and CPU to separate "
            "untimed waiting, blocking or descheduling, and computation. Validation phase timers "
            "can overlap, so their detailed diagnostic panel remains unstacked. Instance is "
            "intentionally ignored."
        ),
    )


def slot_timeline(chain, chain_name, *, id):
    """One chain's median winning-block path, its marks drawn in the order they happen."""
    marks = (("collate_start", "Collation starts"),
             ("collate_finish", "Collation finishes"),
             ("finalize_cert", "Certificate finalizes"),
             ("apply", "Block applies"))
    return chain_timeseries(
        f"Slot timeline — zero is slot start — {chain_name}",
        *[line(label, slot_mark_median(mark, chain=chain)) for mark, label in marks],
        line("slot start", "vector(0)"),
        line("slot end", f"vector({SLOT})"),
        scope="successful winning collations across the selected job", unit="s", id=id,
        w=12, h=8, min=None, query_scope="job",
        axis_label="median seconds from slot start", fill=0, line_width=2,
        styles=[series_style("slot start", width=1, dash=(3, 3), color="gray"),
                reference_line("slot end")],
        description=(
            f"The median local winning-block path on the {chain_name.lower()}, on one common "
            f"clock. This panel is pinned to chain={chain} and " + PINNED_NOTE +
            "Zero is the scheduled start of the slot being collated: negative means pipeline "
            "pre-roll, positive means after slot start, and slot end is the configured budget. "
            "The four marks are listed in the order they happen and tell the story from collation "
            "start through local apply without p95, mean and retained-max overlays. Expand the "
            "detailed consensus row for tails and exact outliers. This pools the rotating "
            "collator population and ignores Instance. This is the collation-side path, and only "
            "that: slot zero belongs to the identity that collated the slot, and all four marks "
            "are ones it observes itself, so every collated round contributes every mark. "
            "Validation is deliberately absent — it is owned by the validating identity, which "
            "holds no slot zero, so a delegated round would contribute the four and not it and "
            "the medians would no longer describe one population. Read validation on the "
            "validate_wait and validate stages, which are portable across identities."
        ),
    )


ROWS = [
    row("Block production at a glance", id=107, panels=[
        collation_glance("master", "Masterchain", id=60),
        collation_glance("shard", "Shardchain", id=65),
        validation_glance("master", "Masterchain", id=61),
        validation_glance("shard", "Shardchain", id=66),
        stacked_timeseries(
            "Collate time stats — mean per block — ${chain:text}",
            *[line(label, collation_phase_mean("real", phase))
              for phase, label in PIPELINE_LABELS],
            line("Other work", collation_other_work("real")),
            line("Wait externals", below_zero(collation_wait("wait_externals"))),
            line("Other wait", below_zero(collation_other_wait())),
            scope="all successful collations in the selected job", unit="s", id=62,
            w=24, h=10, min=None, query_scope="job",
            axis_label="seconds / block (+ work, − waiting)",
            styles=[series_style("Other work", color="gray"),
                    series_style("Wait externals", color="purple"),
                    series_style("Other wait", color="#555555")],
            description=(
                "The devnet-style answer to where collation time goes. Each point is the mean "
                "for one successful block: additive outer work phases stack above slot zero; "
                "explicit external-message waiting and otherwise unattributed waiting stack "
                "below it. Other work is total measured real time minus the named outer phases, "
                "clamped at zero. Nested transaction and preliminary-storage timers are excluded "
                "because adding them here would double count their parent phase. Hover one "
                "timestamp for its complete breakdown. Unlike the chain-pinned summary tables and "
                "slot timelines of this row, this stack follows the Chain selector. It pools "
                "rotating collators across the selected job and intentionally ignores Instance."
            ),
        ),
        stacked_timeseries(
            "Collate CPU stats — mean per block — ${chain:text}",
            *[line(label, collation_phase_mean("cpu", phase))
              for phase, label in PIPELINE_LABELS],
            line("Other CPU", collation_other_work("cpu")),
            scope="all successful collations in the selected job", unit="s", id=63,
            w=24, h=9, query_scope="job", axis_label="CPU seconds / block",
            styles=[series_style("Other CPU", color="gray")],
            description=(
                "Process CPU seconds spent on one successful collation, split across the same "
                "additive outer phases and colors as the wall-time chart above. Other CPU is the "
                "measured total minus named outer phases, clamped at zero. This is CPU cost per "
                "block, not host CPU utilization: compare it with real time above to distinguish "
                "computation from blocking or descheduling. Nested timers are deliberately "
                "excluded. Like the wall-time chart above, this stack follows the Chain selector "
                "rather than pinning a chain. The population is job-wide because collators rotate."
            ),
        ),
        slot_timeline("master", "Masterchain", id=64),
        slot_timeline("shard", "Shardchain", id=67),
    ]),
    row("Chain health history", id=104, collapsed=True, panels=[
        chain_timeseries(
            "Masterchain block rate — history",
            line("chain", masterchain_rate("1m")),
            line("configured target", f"vector(1 / {SLOT})"),
            scope="the whole chain", unit="ops", id=31, w=8, decimals=2,
            query_scope="job",
            axis_label="blocks/s", legend=HISTORY_LEGEND,
            thresholds=thresholds(base="blue"), styles=[reference_line("configured target")],
            description=(
                "Canonical masterchain-head advance over a rolling one-minute window: first take "
                "the highest applied seqno observed across the selected jobs, then calculate its "
                "change. This is one chain-level series, intentionally independent of Instance and "
                "validator count. During a mixed-version rollout, it uses this head only when "
                "every applied-block reporter also exports it; otherwise it falls back to the "
                "median duplicate applied-block stream. The dashed guide is the configured target "
                "(1 / Slot (s)); it does not alter the measured rate. Current chain-health cards "
                "live on TON Overview."
            ),
        ),
        fleet_timeseries(
            "Masterchain age — history", *fleet_spread(MASTERCHAIN_AGE),
            unit="s", id=32, w=8, decimals=1,
            axis_label="age", legend=HISTORY_LEGEND, styles=SPREAD_STYLES,
            thresholds=threshold_lines(30, 120),
            description=(
                "Positive age of the last applied masterchain block across selected reporting "
                "targets: worst (max), p95, median, average, and best (minimum) at each point. A "
                "max-only rise isolates a lagging node; p95/median rising with it points to a "
                "fleet-wide stall. "
                "Missing targets are excluded, and with a small selection p95 nearly follows max. "
                "An offline target disappears; check the Overview offline-target card alongside "
                "this panel. Negative/future clock skew is clamped to zero, so best is not a "
                "clock-skew alarm. Yellow at 30 s and red at 120 s are conservative "
                "deployment-neutral guides; tighten them for faster networks; TON Overview owns "
                "the current worst-value card."
            ),
        ),
        chain_timeseries(
            "Shardchain aggregate block rate — history", line("chain", shard_rate("1m")),
            scope="the median node", unit="ops", id=33, w=8, decimals=2,
            query_scope="job",
            axis_label="blocks/s", legend=HISTORY_LEGEND, thresholds=thresholds(base="blue"),
            description=(
                "Canonical shard blocks applied over a rolling one-minute window, aggregated "
                "across active shards. There is no single shardchain seqno, so the median only "
                "reconciles duplicate observations from healthy nodes; it does not average "
                "different chains. This chain-level series intentionally ignores Instance and "
                "validator count; TON Overview owns the current chain-rate card."
            ),
        ),
        fleet_timeseries(
            "Shard lag — history", *fleet_spread(SHARD_LAG),
            unit="short", id=34, w=8, decimals=0,
            axis_label="MC blocks", legend=HISTORY_LEGEND, styles=SPREAD_STYLES,
            thresholds=threshold_lines(5, 20),
            description=(
                "Masterchain blocks by which the shard client trails, shown as worst (max), p95, "
                "median, average, and best (minimum) across selected reporting targets at each "
                "point. A max-only rise isolates one lagging node; p95/median rising with it means basechain "
                "processing is broadly behind. Missing targets and targets lacking either gauge "
                "are excluded; with a small selection p95 nearly follows max. An offline target "
                "disappears; check the Overview offline-target card alongside this panel. "
                "Sustained growth makes basechain state stale even when masterchain age is "
                "healthy; TON Overview owns the current worst-value card."
            ),
        ),
        chain_timeseries(
            "Shard collation budget — history", line("value", collation_budget("1m")),
            scope="every collator in the fleet", unit="percentunit", id=35, w=8, decimals=0,
            query_scope="job",
            axis_label="slot budget", legend=HISTORY_LEGEND, color_mode="thresholds",
            thresholds=threshold_lines(0.75, 1),
            description=(
                "Elapsed collation time per produced shard block over a rolling one-minute window, "
                "divided by the configured Slot (s). Numerator and denominator are fleet sums "
                "because exactly one node collates each block. This chain-wide signal follows Job "
                "but intentionally ignores Instance because collators rotate. At 100%, collation "
                "alone exhausts the configured slot. TON Overview owns the current card; this "
                "panel shows the signal over the selected dashboard range. Excludes "
                "wait_externals: time awaiting the external-message queue, including deliberate "
                "slot pacing and queue fetches. It is idle wait rather than measured collation "
                "work; on a fast build it can dominate elapsed time and fake a full budget. No "
                "successful collation in the window renders No data."
            ),
        ),
        chain_timeseries(
            "Shard candidates discarded (10m) — history", line("value", discarded_share("10m")),
            scope="the whole chain", unit="percentunit", id=36, w=8, decimals=0,
            query_scope="job",
            axis_label="discarded share", legend=HISTORY_LEGEND, color_mode="thresholds",
            thresholds=threshold_lines(0.15, 0.3),
            description=(
                "Fleet-wide 10-minute estimate of successfully collated shard candidates that did "
                "not become applied blocks. Applied rate is the median across reporting nodes; "
                "successful collations are summed across rotating collators. This signal follows "
                "the job selector but intentionally ignores the instance selector, because "
                "selecting only part of the rotating collator set makes the comparison invalid. No "
                "successful collation or incomplete collated-block coverage during a rolling "
                "upgrade renders No data, not a false 0%. TON Overview owns the current card; "
                "this panel shows the signal over the selected dashboard range."
            ),
        ),
    ]),
    row("Block flow & production outcomes", id=101, collapsed=True, panels=[
        chain_timeseries(
            "Block flow (chain-wide) — ${chain:text}",
            line("node-applied (replica median)",
                 f"quantile(0.5, sum by (job, instance) (rate(ton_first_received_total"
                 f"{sel(**JOB, workchain=CHAIN)}[{RATE}])))"),
            line("collated {{result}}", summed("result", blocks())),
            line("validated {{result}} / validator",
                 f"{summed('result', blocks('validated'))} / scalar(clamp_min(count("
                 f"max by (job, instance) (ton_validator_groups"
                 f"{sel(**JOB, chain=CHAIN)} > 0)), 1))"),
            scope="every reporter in the selected job", unit="ops", id=5, h=9,
            query_scope="job",
            axis_label="blocks/s (events/s for validated)",
            styles=[series_style("node-applied (replica median)", color="blue"),
                    series_style("collated ok", color="green"),
                    series_style("validated ok / validator", color="purple"),
                    series_style("collated error", color="red"),
                    series_style("validated error / validator", color="orange")],
            description=(
                "Chain-wide production flow for the selected jobs; this panel intentionally "
                "ignores Instance because collators rotate. The node-applied line uses a median "
                "only to reconcile duplicate observer streams; unlike the primary masterchain rate "
                "above, it can expose local apply timing during lag or catch-up. Collations are "
                "summed across rotating collators, and validations are divided by active "
                "validators so the line stays comparable when validator count changes. A "
                "persistent collated-ok gap above node-applied estimates discarded candidates."
            ),
        ),
        fleet_timeseries(
            "Production problems (% of blocks) — ${chain:text}",
            attributed("collation errors", error_share("collated", "job, instance"),
                       shown=error_share("collated"),
                       overlay=error_contribution("collated")),
            attributed("validation errors", error_share("validated", "job, instance"),
                       shown=error_share("validated"),
                       overlay=error_contribution("validated")),
            line("overload: {{reason}}",
                 f"{summed('reason', collation_rate('ton_collation_overload_total'))}"
                 f" / on () group_left () ({summed(None, blocks(result='ok'))} > 0) > 0"),
            line("want_split",
                 f"{per_block(summed(None, collation_rate('ton_collation_want_split_total')))}"
                 f" > 0"),
            unit="percentunit", id=6, h=9, max=1,
            axis_label="events/s", thresholds=failure_bands(),
            styles=[series_style("collation errors", color="red"),
                    series_style("validation errors", color="orange")],
            description=(
                "Problems as a share of blocks — not durations and not raw event rates. "
                "Collation/validation errors are failed shares of attempts, drawn chain-wide "
                "because one rotating collator produces each block. Their hidden attribution "
                "rows decompose the drawn value: one row per node currently erring, each showing "
                "that node's slice of the chain-wide share, so the rows sum to the drawn line and "
                "a clean node never appears. overload: "
                "long_collation marks blocks that hit the collator deadline. want_split is a "
                "weighted-history decision; overload reasons are only the current block's "
                "contribution, not a one-to-one cause. Durations are below. Scope: chain-wide sums "
                "across the selected jobs; Instance is intentionally ignored because collators "
                "rotate."
            ),
        ),
    ]),
    *production_diagnostic_rows([
        chain_timeseries(
            "Collation latency history (5m) — ${chain:text}",
            *[line(f"{clock} p{int(q * 100)}", processing_quantile("collate", clock, q))
              for clock in ("elapsed", "real", "cpu") for q in (0.5, 0.95)],
            line("slot budget", f"vector({SLOT})"),
            scope="successful collations across every collator", unit="s", id=8,
            query_scope="job", axis_label="seconds / collation",
            fill=0, line_width=2,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            styles=[reference_line("slot budget")],
            description=(
                "Actual event distributions over a fixed five-minute window, pooled across the "
                "rotating collators: median and p95 end-to-end elapsed, attributed real work, and "
                "process CPU time for one successful collation. These are six independent "
                "quantiles and are never stacked or added. Elapsed includes wait_externals; real "
                "measures instrumented wall time and CPU measures execution cost. A widening "
                "elapsed-to-real gap points to waits outside the timed work. Real rising while "
                "CPU stays flat identifies blocking or descheduling inside timed scopes (on macOS, "
                "fsync is a common cause); CPU rising with real points to computation. The dashed line is the "
                "configured slot budget, not a measured sample. Use the retained-max and phase "
                "panels below to identify the node and cause behind a p95 excursion. Failed "
                "attempts, including discarded internal retries, are excluded from these success "
                "tails but included in the CPU-load panel below; error CPU is best-effort when a "
                "timer scope is still active at failure reporting."
            ),
        ),
        stacked_timeseries(
            "Collation nested work (per block) — ${chain:text}",
            line("{{phase}}", share_of_block(summed("phase", stage(
                "collate", clock="real", result="ok", phase=TRANSACTIONS)))),
            line("transaction total", per_block(collated_seconds("real", TRANSACTIONS))),
            line("preliminary storage",
                 per_block(collated_seconds("real", "prelim_storage_stat"))),
            scope="every collator in the fleet", unit="s", id=37, w=8,
            query_scope="job",
            axis_label="seconds / block",
            styles=[unstacked_total("transaction total", "dark-red"),
                    unstacked_total("preliminary storage", "light-blue", dash=(6, 4))],
            description=(
                "Nested collation work that must not be added to the outer pipeline stack. The "
                "transaction TVM, storage-stat, and other timers form an additive transaction "
                "breakdown; transaction total is its top boundary. Preliminary storage is an "
                "independently nested hotspot and is drawn as a dashed line rather than stacked. "
                "No successful collation in the window renders No data."
            ),
        ),
        chain_timeseries(
            "Validation latency history (5m) — ${chain:text}",
            *[line(f"{clock} p{int(q * 100)}", processing_quantile("validate", clock, q))
              for clock in ("elapsed", "real", "cpu") for q in (0.5, 0.95)],
            line("slot budget", f"vector({SLOT})"),
            scope="successful validations across every validator", unit="s", id=56,
            query_scope="job", axis_label="seconds / validation",
            fill=0, line_width=2,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            styles=[reference_line("slot budget")],
            description=(
                "Actual validation event distributions over a fixed five-minute window, pooled "
                "across validators: median and p95 end-to-end elapsed, attributed real work and "
                "process CPU time. These are independent quantiles and are never stacked or "
                "added. A widening elapsed-to-real gap points to waits outside timed work; real "
                "rising while CPU stays flat points to blocking or descheduling inside timed "
                "scopes; CPU rising with real points to computation. Use the retained-max and phase panels "
                "below to identify the node and cause behind a p95 excursion."
            ),
        ),
        agg_timeseries(
            "Collation real/CPU phases by node — ${chain:text}",
            agg_line(phase_per_node("collate", PIPELINE, "collated"), key="clock, phase"),
            unit="s", id=47, h=9,
            axis_label="seconds / block", fill=0, line_width=1,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Which collator is slowest in which real or CPU phase: each node's own seconds in a phase per "
                "block it collated, collapsed across nodes by the Node aggregation switch — worst "
                "by default, so a line is the slowest collator's cost for that phase and the "
                "hidden ⇒ row names it. The total-tail and retained-max panels show how bad a "
                "collation was; this says where its real or CPU cost landed and on whom. Only one "
                "node collates each block, so a node contributes only "
                "while it holds the slot and the series are sparse during rotation — a phase with "
                "no successful collation on any node in the window renders no line. Deliberately "
                "not stacked: these are separate nodes' readings and do not add."
            ),
        ),
        agg_timeseries(
            "Validation real/CPU phases by node — ${chain:text}",
            agg_line(phase_per_node("validate", VALIDATION, "validated"), key="clock, phase"),
            unit="s", id=48, h=9,
            axis_label="seconds / validation", fill=0, line_width=1,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Which validator is slowest in which real or CPU phase: each node's own seconds in a phase per "
                "block it validated, collapsed across nodes by the Node aggregation switch — worst "
                "by default, with the hidden ⇒ row naming that node. Every validator validates "
                "every block, so unlike the collation twin this population is dense and the "
                "spread between worst and median is a real per-node comparison rather than an "
                "artefact of who held the slot. CPU distinguishes computation from wall-time "
                "blocking. Validation phases can run in parallel contexts, so "
                "they do not add up to the per-block total on the panel above and are never "
                "stacked."
            ),
        ),
        fleet_timeseries(
            "Collation CPU load across nodes — ${chain:text}",
            *fleet_spread(processing_cpu_load("collate")),
            unit="short", id=49, h=9,
            axis_label="CPU s/s", fill=0, line_width=2,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            styles=SPREAD_STYLES,
            description=(
                "CPU seconds spent collating per wall second, shown as worst, p95, median, average, "
                "and best across selected nodes. One means one CPU core continuously consumed; "
                "values can exceed one when work runs in parallel. A worst-only rise identifies a "
                "hot collator and the hidden attribution row names it; a fleet-wide rise moves the "
                "median too. This is CPU cost, not elapsed time, and therefore excludes pure fsync "
                "or scheduling waits. Successful, failed, and discarded retry attempts all "
                "contribute; an error scope still active during reporting can be omitted."
            ),
        ),
        fleet_timeseries(
            "Validation CPU load across nodes — ${chain:text}",
            *fleet_spread(processing_cpu_load("validate")),
            unit="short", id=50, h=9,
            axis_label="CPU s/s", fill=0, line_width=2,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            styles=SPREAD_STYLES,
            description=(
                "CPU seconds spent validating per wall second, shown as worst, p95, median, "
                "average, and best across selected validators. One means one CPU core continuously "
                "consumed. The worst line's hidden row names the range-dominant node. Compare CPU "
                "with elapsed and real tails: real rising without CPU indicates blocking or "
                "descheduling, while elapsed rising without real indicates untimed waits. CPU "
                "rising with real indicates expensive validation work."
            ),
        ),
        fleet_timeseries(
            "Failed/retry collation maxima — ${chain:text}",
            *[attributed(f"{clock} failed/retry max",
                         processing_recent_max("collate", clock, "error"))
              for clock in ("elapsed", "real", "cpu")],
            unit="s", id=51, h=9,
            axis_label="seconds / collation", fill=0,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Exact maxima for failed or discarded internal collation attempts, retained by "
                "each node for roughly 10–20 minutes and collapsed to the fleet worst with stable "
                "node attribution. Successful maxima already live in the compact summary above; "
                "this panel exists only so expensive retries cannot disappear from success-only "
                "p50/p95. Elapsed, real and CPU maxima may belong to different nodes and attempts. "
                "Error real/CPU are best-effort when a timer scope was still active at reporting."
            ),
        ),
        fleet_timeseries(
            "Failed validation maxima — ${chain:text}",
            *[attributed(f"{clock} failed max",
                         processing_recent_max("validate", clock, "error"))
              for clock in ("elapsed", "real", "cpu")],
            unit="s", id=52, h=9,
            axis_label="seconds / validation", fill=0,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Exact failed-validation maxima retained per node for roughly 10–20 minutes. The "
                "visible lines are the fleet worst and hidden rows name their validators. "
                "Successful maxima already live in the compact summary above, so this panel only "
                "preserves pathological failures that a success distribution cannot show. Compare "
                "elapsed, real and CPU to separate untimed waits, blocking and computation. Error "
                "real/CPU are best-effort when a timer scope was still active."
            ),
        ),
        agg_timeseries(
            "Collation phase outliers — ${chain:text}",
            agg_line(phase_recent_max("collate", "~" + "|".join(PIPELINE_ORDER)),
                     key="result, clock, phase"),
            unit="s", id=53, h=10,
            axis_label="retained max seconds", fill=0, line_width=1,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Retained exact maximum for every outer collation phase, split by success/error "
                "and real/CPU clock, then collapsed across nodes by Node aggregation. Failed "
                "includes discarded internal retries. Worst is the default and the hidden row "
                "names the collator. This is the causal companion to the total retained maximum, "
                "but each phase maximum may come from a different attempt and must not be read as "
                "a decomposition of that worst total event. Error phases are best-effort when a "
                "timer scope was still active at reporting. A large "
                "real-but-not-CPU phase suggests blocking, while both rising suggests computation. "
                "The outer phases are additive per attempt; nested transaction "
                "and preliminary-storage timers remain excluded."
            ),
        ),
        agg_timeseries(
            "Validation phase outliers — ${chain:text}",
            agg_line(phase_recent_max("validate", VALIDATION), key="result, clock, phase"),
            unit="s", id=54, w=24, h=10,
            axis_label="retained max seconds", fill=0, line_width=1,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Retained exact maximum for every validation phase, split by success/error and "
                "real/CPU clock, then collapsed across nodes by Node aggregation. Worst is the "
                "default and the hidden row names the validator. Each phase maximum may come from "
                "a different validation, and phase timers can overlap or run in parallel; these "
                "are therefore independent outlier signals, never a stack or an additive "
                "decomposition of the worst total event. Error phases are best-effort when a "
                "timer scope was still active at reporting."
            ),
        ),
        chain_timeseries(
            "Work per block — ${chain:text}",
            line("transactions / block",
                 per_block(summed(None, collation_rate("ton_collation_transactions_total")))),
            line("externals offered / block",
                 per_block(summed(None,
                                  collation_rate("ton_collation_ext_messages_offered_total")))),
            line("gas / block",
                 per_block(summed(None, collation_rate("ton_collation_gas_total")))),
            scope="every collator in the fleet", unit="short", id=23, w=8, h=9,
            query_scope="job",
            axis_label="per block",
            styles=[series_style("gas / block", axis="right", axis_label="gas / block")],
            description=(
                "Work carried by an average successfully collated block for the selected chain. "
                "Transactions and externals offered use the left axis; gas uses the right. Offered "
                "means presented to the collator, not necessarily included. Flat work with a "
                "falling block rate points to time or coordination rather than block load. No "
                "successful collation in the window renders No data."
            ),
        ),
        chain_timeseries(
            "Bytes per block — ${chain:text}",
            line("block bytes",
                 per_block(summed(None, collation_rate("ton_collation_block_bytes_total")))),
            line("collated data bytes",
                 per_block(summed(None,
                                  collation_rate("ton_collation_collated_data_bytes_total")))),
            scope="every collator in the fleet", unit="bytes", id=24, w=8, h=9,
            query_scope="job",
            axis_label="bytes / block",
            description=(
                "Average serialized block and collated-data size for the selected chain. If "
                "collated data grows, correlate it with create_collated_data in the phase panel. "
                "No successful collation in the window renders No data."
            ),
        ),
        agg_timeseries(
            "Block/state/storage perf load",
            agg_line(rate("ton_perf_op_ticks_total", "op, job, instance",
                          per_tick=True, op=STORAGE_OPS), key="op", name="{{op}}"),
            unit="short", id=38, w=24, h=10,
            axis_label="elapsed s/s", fill=0, line_width=1,
            legend=table_legend("lastNotNull", "max", sort="Max", placement="right"),
            description=(
                "Per-node TD_PERF_COUNTER elapsed load, collapsed with Node aggregation, for "
                "consensus-signature and low-level block, state, cell, CellDB, RocksDB commit, "
                "and file-sync operations relevant to "
                "production stalls. One unit means one scope-equivalent continuously elapsed on a "
                "node; blocking and descheduling count, so this is not OS CPU load. Up to 17 "
                "registered matching operations are shown without top-k truncation; untouched "
                "sites have no series. Scopes may be nested and also cover work outside "
                "collation/validation, so do not add the lines and correlate them with the phase "
                "panels above."
            ),
        ),
        chain_timeseries(
            "First slot vs steady state — ${chain:text}",
            line("first in window: elapsed / collation",
                 first_window_per_block("ton_collation_elapsed_seconds_total", "1")),
            line("steady state: elapsed / collation",
                 first_window_per_block("ton_collation_elapsed_seconds_total", "0")),
            line("first in window: transactions / block",
                 first_window_per_block("ton_collation_transactions_total", "1")),
            line("steady state: transactions / block",
                 first_window_per_block("ton_collation_transactions_total", "0")),
            scope="every collator in the fleet", unit="s", id=70, h=9,
            query_scope="job", axis_label="seconds / collation",
            styles=[series_style("first in window: transactions / block", unit="short",
                                 axis="right", axis_label="transactions / block", dash=(6, 4)),
                    series_style("steady state: transactions / block", unit="short",
                                 axis="right", dash=(6, 4))],
            description=(
                "Cost of the leader-window boundary. The first slot of a producer's window "
                "cannot pipeline over the previous producer's handoff and inherits whatever "
                "backlog that producer left, so its collations run longer and carry more work. "
                "Solid lines are mean end-to-end elapsed per successful collation; dashed lines "
                "(right axis) are transactions per block. Collation paths that cannot know their "
                "slot report as steady state. Chain-wide across rotating collators; intentionally "
                "ignores Instance."
            ),
        ),
        chain_timeseries(
            "Out-msg queue — ${chain:text}",
            line("cleaned / block", queue_progress("ton_collation_out_queue_cleaned_total")),
            line("processed / block", queue_progress("ton_collation_out_queue_processed_total")),
            line("skipped (already handled) / block",
                 queue_progress("ton_collation_out_queue_skipped_total")),
            line("queue depth after collation",
                 f"max(ton_collation_out_queue_size{sel(**JOB, chain=CHAIN)})"),
            scope="every collator in the fleet", unit="short", id=68, h=9,
            query_scope="job", axis_label="messages / block",
            styles=[series_style("queue depth after collation", axis="right",
                                 axis_label="queue depth", color="red")],
            description=(
                "Out-msg queue backlog and drainage. Cleaned counts delivered messages the "
                "cleanup pass removed; processed and skipped count inbound neighbor messages "
                "consumed or recognized as already handled — skipped is not a cleanup leftover "
                "tally. Per-block rates cover successful final attempts only. Queue depth (right "
                "axis) is each collator's post-collation snapshot shown as the maximum across "
                "reporters; a node that has not collated recently holds a stale snapshot. A depth "
                "that grows while cleaned stays flat means production outpaces delivery."
            ),
        ),
        chain_timeseries(
            "Storage-stat cache — ${chain:text}",
            line("{{outcome}} lookups / block",
                 share_of_block(summed("outcome", collation_rate(
                     "ton_collation_storage_cache_lookups_total")))),
            line("{{outcome}} cells / block",
                 share_of_block(summed("outcome", collation_rate(
                     "ton_collation_storage_cache_cells_total")))),
            scope="every collator in the fleet", unit="short", id=69, h=9,
            query_scope="job", axis_label="lookups / block",
            styles=[series_style(f"{outcome} cells / block", axis="right",
                                 axis_label="cells / block", dash=(6, 4))
                    for outcome in ("hit", "miss", "small")],
            description=(
                "Storage-stat cache effectiveness during collation: lookups (solid) and the "
                "cells they covered (dashed, right axis) per successfully collated block, split "
                "by hit, miss, and small — accounts below the caching threshold. Sustained misses "
                "alongside a large final-storage-stats phase in the pipeline panel point at cache "
                "sizing. Successful final attempts only; chain-wide across rotating collators."
            ),
        ),
    ]),
    row("Consensus & finality", id=106, collapsed=True, panels=[
        chain_timeseries(
            "Slot outcomes (5m) — ${chain:text}",
            line("terminal cadence / target", f"({round_terminal_rate()}) * {SLOT}"),
            line("{{outcome}} share", round_outcome_share()),
            scope="the average validator group in the selected job", unit="percentunit", id=43,
            w=24, h=8, query_scope="job", min=0, max=1.1,
            axis_label="share of target / terminal slots", fill=0, line_width=2,
            styles=[series_style("terminal cadence / target", color="blue", width=3),
                    series_style("accepted share", color="green"),
                    series_style("empty share", color="orange"),
                    series_style("skipped share", color="red")],
            description=(
                "Whether slots reach a certified terminal outcome, and how those outcomes split. "
                "Terminal cadence is accepted + empty + skipped per validator group, divided by "
                "the configured target rate; 100% means the group finalizes at slot cadence. "
                "Accepted carried a full block, empty certified no block, and skipped certified "
                "that no candidate was accepted. Recovery can delay an accepted/empty increment "
                "until both candidate kind and its completion boundary are known, so a short window "
                "can dip and later catch up. "
                "Rates use a fixed five-minute window, normalize "
                "each validator by its active group count, then average validators so replica and "
                "shard counts cannot inflate the result. Only validator identities count rounds, "
                "so the numerator matches the validator group count it is divided by even on a "
                "node that also runs collator or observer identities for the same group."
            ),
        ),
        summary_table(
            "Finality position in slot (5m) — ${chain:text}",
            "mark",
            [
                summary_column(
                    "p95 of slot",
                    f"({slot_mark_quantile('after', 0.95, '~finalize_cert|apply')}) / {SLOT}",
                    unit="percentunit",
                ),
                summary_column(
                    "recent latest of slot",
                    f"({slot_mark_recent_max_summary('after', '~finalize_cert|apply')}) / {SLOT}",
                    unit="percentunit",
                ),
            ],
            id=41, w=24, h=5, key_name="Event",
            key_mappings=(("finalize_cert", "Finality certificate"),
                          ("apply", "Block applied")),
            label_columns=(("max_owner", "latest owner"),), query_scope="job",
            field_order=("Event", "p95 of slot", "recent latest of slot", "latest owner"),
            description=(
                "How late finality and local apply land in the slot without redrawing the median "
                "timeline above. Values are positions after slot start divided by Slot (s): 100% "
                "is slot end and anything above it overran the configured slot. p95 pools winning "
                "full-block events over a fixed five-minute window; early/on-time observations "
                "are zero-padded on this one-sided distribution. Recent latest is an exact "
                "maximum retained for roughly 10–20 minutes and names its collator. Empty and "
                "skipped slots are represented in Slot outcomes instead."
            ),
        ),
        summary_table(
            "Consensus stage stats (5m) — ${chain:text}",
            "stage",
            [
                summary_column("p50", stage_quantile(0.5), unit="s"),
                summary_column("p95", stage_quantile(0.95), unit="s"),
                summary_column("slowest mean", slowest_stage_mean(), unit="s"),
                summary_column("samples", slowest_stage_samples(), unit="short"),
                summary_column("recent max", stage_recent_max_summary(), unit="s"),
            ],
            id=55, w=24, h=10, key_name="Stage",
            key_mappings=(("collate", "Collation"), ("publish", "Publish candidate"),
                          ("validate_wait", "Wait to validate"), ("validate", "Validation"),
                          ("notarize_vote", "Notarize vote"),
                          ("notarize_cert", "Notarize certificate"),
                          ("finalize_vote", "Finalize vote"),
                          ("finalize_cert", "Finalize certificate"),
                          ("apply", "Apply block")),
            label_columns=(("slow_owner", "slow node"), ("max_owner", "max owner")),
            field_order=("Stage", "p50", "p95", "slowest mean", "slow node", "samples",
                         "recent max", "max owner"),
            sort_by="p95", sort_desc=True, query_scope="job",
            description=(
                "One row per independently observed consensus stage. p50/p95 are event "
                "distributions pooled over a fixed five-minute window. Slowest mean names the "
                "node with the highest five-minute mean and Samples shows how much evidence backs "
                "that comparison. Recent max is an exact single observation retained for roughly "
                "10–20 minutes with its owner. Stage populations differ — collation/publish are "
                "collator-only and later stages come from participating validators — so rows must "
                "not be added into a synthetic round. Certificate stages include signing, quorum "
                "wait, local persistence and possible fsync, not pure network latency."
            ),
        ),
        summary_table(
            "Slot handoff stats (5m) — ${chain:text}",
            "side",
            [
                summary_column("p50", handoff_quantile_summary(0.5), unit="s"),
                summary_column("p95", handoff_quantile_summary(0.95), unit="s"),
                summary_column("recent max", handoff_recent_max_summary(), unit="s"),
            ],
            id=42, w=24, h=5, key_name="Handoff",
            key_mappings=(("dead_time", "Dead time"), ("head_start", "Head start")),
            label_columns=(("max_owner", "max owner"),), query_scope="job",
            field_order=("Handoff", "p50", "p95", "recent max", "max owner"),
            description=(
                "The two mutually exclusive sides of the handoff around prior-slot local "
                "completion. Dead time means the next collation started afterward; head start "
                "means it was already running. Every handoff contributes to both histograms with "
                "the opposite side recorded as zero, but p95s and maxima must not be subtracted. "
                "Recent max is exact for roughly 10–20 minutes and names its collator. A large "
                "masterchain dead time can be healthy unused slot budget on a fast chain; rising "
                "head start indicates pipelining. Full-block completion includes apply/storage "
                "and fsync; empty/skipped completion is the certificate."
            ),
        ),
    ]),
    row("Block propagation", id=105, collapsed=True, panels=[
        chain_timeseries(
            "First-arrival winner / applied blocks (recent) — ${chain:text}",
            line("{{source}}", by_source("ton_first_received_total")),
            scope="all selected nodes", unit="percentunit", id=39, w=24, h=10,
            decimals=1, max=1, axis_label="share of applied blocks", fill=5,
            legend=SOURCE_LEGEND,
            description=(
                "Which source reached each selected node first for blocks that later became "
                "applied. Each node contributes one winner per applied block; the query uses "
                "Grafana's scrape-safe recent rate window and then averages nodes, so changing "
                "validator count does not inflate it. The window is normally about 20 seconds for "
                "a 5-second scrape and one minute for a 15-second scrape. The legend is sorted by "
                "mean: the first source usually won the same-block race. This is not latency in "
                "seconds, and candidate receipt can naturally win before finality. Download, local "
                "acceptance, and unknown remain visible as fallback paths."
            ),
        ),
        chain_timeseries(
            "Source arrivals / applied blocks (recent) — ${chain:text}",
            line("{{source}}", by_source("ton_received_total")),
            scope="all selected nodes", unit="percentunit", id=40, w=24, h=10,
            decimals=1, axis_label="arrivals / applied block", fill=5,
            legend=SOURCE_LEGEND,
            description=(
                "How often each distinct source was observed per applied block over Grafana's "
                "scrape-safe recent rate window, computed per node and then averaged. The window "
                "is normally about 20 seconds for a 5-second scrape and one minute for a 15-second "
                "scrape. 100% means a node usually saw that source for every applied block. "
                "Sources overlap, so the lines need not sum to 100%. Late arrivals count only "
                "while the block remains in the 1000-entry receive-stat LRU, and window boundaries "
                "can briefly push a source above 100%. High coverage with a low first-arrival "
                "share means the source usually loses the race."
            ),
        ),
    ]),
    *external_message_rows([
        plain_stat(
            "Validated/admitted /s (global)", accepted("1m"),
            id=25, scope="all selected nodes", unit="ops", decimals=1, w=8,
            thresholds=thresholds(base="blue"),
            description=(
                "Successful external-validation rate over the last minute, evaluated now and "
                "averaged across nodes. On storing nodes it combines new accepted pool entries with "
                "stock-neutral priority upgrades; on non-storing nodes validated_only keeps the "
                "successful observation visible. Legacy accepted counters included the non-storing "
                "case while reprioritized was already separate, so this three-outcome sum preserves "
                "the historical successful-check meaning during a rolling upgrade. "
                "Admission has no chain label, so compare it with selected-chain execution only as "
                "context."
            ),
        ),
        plain_stat(
            "Applied /s (synced) — ${chain:text}", synced_applied("1m"),
            id=26, scope="the median synced node", unit="ops", decimals=1, w=8,
            query_scope="job",
            thresholds=thresholds(base="blue"),
            description=(
                "External messages observed in applied blocks for the selected chain over the last "
                "minute. The median excludes reporters whose absolute masterchain age is at least "
                "120 seconds; shardchain additionally requires shard lag of at most two blocks. "
                "This limits replay spikes from syncing nodes and excludes future-skewed chain "
                "clocks. The counter is still a replicated node observation and includes catch-up "
                "replay; it is not an objective protocol-wide chain counter."
            ),
        ),
        worst_node_stat(
            "Eligible mempool backlog",
            mempool.eligible(),
            id=27, drill=SELF, unit="locale", w=4,
            thresholds=thresholds(("yellow", 1000), ("red", 5000)),
            description=(
                "External messages counted as eligible in the busiest selected node-local pool: "
                "stored entries whose active flag is true, across every destination chain and "
                "priority. This does not promise that the current collator can select every entry. "
                "Green below 1k, "
                "yellow at 1k, red at 5k; tune thresholds to the deployment. Stateful exporters "
                "exclude postponed storage, which can remain inactive past its retry deadline until "
                "a collator snapshot revisits it; legacy unlabeled history falls back "
                "to the old all-stored count. Use the state, reconciliation, and removal panels "
                "below to explain growth."
            ),
        ),
        worst_node_stat(
            "Oldest pending external",
            mempool.oldest_age(),
            id=28, drill=SELF, unit="s", decimals=0, w=5,
            thresholds=thresholds(("yellow", 60), ("red", 300)),
            description=(
                "Age of the oldest stored external message on the worst selected node, across all "
                "destination chains, priorities, and active/postponed states. Messages carry a 600s "
                "TTL: red (300s and beyond) means messages are about to expire unserved; values "
                "at or past 600s mean atomic deadline-driven expiry processing is late and appear "
                "as expiry-handler lag below. Reprioritization resets an entry's age and expiry "
                "deadline. Only stateful exporters are shown; legacy nodes are omitted because "
                "their periodic expiry semantics are not comparable."
            ),
        ),
        plain_stat(
            "External work in failed attempts (10m)",
            f"{summed(None, collated_externals('10m', outcome='included', result='error'))}"
            f" / ({summed(None, collated_externals('10m', outcome='included'))} > 0)",
            id=29, scope="every collator in the fleet", unit="percentunit", decimals=0, w=8,
            query_scope="job",
            thresholds=thresholds(("yellow", 0.15), ("red", 0.3)),
            description=(
                "10-minute share of selected-chain external executions that occurred in failed "
                "collation attempts. This measures work discarded by automatic retries; successful "
                "candidates later rejected by consensus are not included. No external execution in "
                "the window renders No data."
            ),
        ),
        chain_timeseries(
            "External flow /s — ${chain:text}",
            line("validated/admitted (per node avg)", accepted(RATE)),
            line("included in candidate (ok attempts)",
                 summed(None, collated_externals(outcome="included", result="ok"))),
            line("applied observation (synced median)", synced_applied(RATE)),
            scope="the mempool and collators of every selected node", unit="ops", id=10, h=9,
            query_scope="mixed",
            axis_label="messages/s",
            styles=[series_style("validated/admitted (per node avg)", color="blue"),
                    series_style("included in candidate (ok attempts)", color="yellow"),
                    series_style("applied observation (synced median)", color="green")],
            description=(
                "Correlated stages, not a conservation funnel. Validated/admitted is the global "
                "successful-validation rate averaged across nodes: storing nodes count new entries "
                "and priority upgrades, while non-storing nodes report validated_only. Included is "
                "selected-chain work in successful candidates; "
                "failed-attempt execution is shown next door. Applied is the median replicated "
                "observation from reporters whose absolute masterchain age is under 120 seconds; "
                "shardchain additionally requires shard lag of at most two blocks. It still "
                "includes catch-up replay and is not an objective chain counter. Use backlog and "
                "local removals to distinguish waiting from eviction."
            ),
        ),
        chain_timeseries(
            "External admission & collation outcomes /s — ${chain:text}",
            line("admission/storage: {{outcome}}",
                 f"{summed('outcome', rejections(), agg='avg')} > 0"),
            line("collation: {{outcome}}",
                 f"{summed('outcome', collated_externals(outcome='!included'))} > 0"),
            line("collation: included in failed attempt",
                 f"{summed(None, collated_externals(outcome='included', result='error'))} > 0"),
            scope="every selected node", unit="ops", id=11, h=9, axis_label="messages/s",
            query_scope="mixed",
            description=(
                "Admission/storage outcomes are global per-node averages and respect Instance; "
                "accepted and validated_only successes, plus stock-neutral reprioritized and "
                "duplicate outcomes, are excluded here. pool_full and address_full passed "
                "validation but were not stored locally. "
                "Collation outcomes are selected-chain sums across all collators in the selected "
                "jobs and intentionally ignore Instance: rejected did not make it into the "
                "candidate (normally a TVM rejection or an attempt-aborting processing failure), "
                "filtered failed registration, skipped_backpressure stayed pending, and included "
                "in failed attempt is discarded execution work. Zero-valued series are hidden."
            ),
        ),
        agg_timeseries(
            "Local mempool removals /s (5m, per reason)",
            agg_line(removals(), key="reason", name="{{reason}}"),
            unit="ops", id=30, h=9,
            axis_label="messages/s",
            styles=[
                series_style("applied", color="green"),
                series_style("expired", color="red"),
            ],
            description=(
                "Why entries left selected pools. Applied is normal cleanup after observing the "
                "message on chain; every other reason is a local eviction: expired hit the 600s "
                "TTL, rejected_final exhausted its ~3 retry postpones, pool_pressure was evicted "
                "while its priority level was at the soft limit, and filtered could not be "
                "registered by collation (for example, duplicate or wrong shard). Five-minute "
                "rates smooth bursty arrivals and removals without turning individual deadline "
                "expiries into misleading scrape-sized spikes. Sustained expired means at least "
                "one selected pool failed to observe messages applied before their TTL; "
                "correlate with applied throughput and other nodes before calling it network-wide "
                "loss. The Node aggregation switch chooses the pointwise worst, p95, median, or "
                "best selected node independently per reason; the lines need not come from one "
                "pool."
            ),
        ),
        agg_timeseries(
            "External mempool state",
            agg_line(mempool.eligible(), name="eligible backlog"),
            agg_line(mempool.total(), name="total stored"),
            agg_line(mempool.expiry_handler_lag(), name="expiry handler lag"),
            unit="locale", id=12, h=9,
            axis_label="messages",
            styles=[
                series_style("eligible backlog", color="blue", width=3),
                series_style("total stored", color="purple", dash=(6, 4), fill=0),
                series_style(r"^expiry handler lag(?: ⇒ .*)?$", regex=True,
                             unit="s", axis="right",
                             axis_label="expiry lag", color="red", fill=0),
            ],
            description=(
                "The Node aggregation switch chooses the pointwise worst, p95, median, or best "
                "selected node independently per series. Every value is one node's pool across all "
                "destination chains and priorities, never a fleet sum. Eligible means a stored entry "
                "whose active flag is true, not a guarantee that the current collator can select it. "
                "Total stored also includes postponed entries whose active flag is false, so its gap "
                "above eligible is exactly postponed storage. A postponed entry can remain in that "
                "state after its retry deadline until a collator snapshot revisits and reactivates "
                "it. Expiry is removed atomically at its 600-second deadline; the right-axis expiry "
                "handler lag is max(oldest age - 600s, 0) and should be zero. It is limited to "
                "stateful exporters so legacy expiry semantics cannot win the Node aggregation. "
                "Legacy unlabeled history appears as both eligible fallback and total because the "
                "split is unknown."
            ),
        ),
        agg_timeseries(
            "Mempool stock / accounted flow (5m change)",
            agg_line(mempool.stock_delta(), name="measured stock delta"),
            agg_line(mempool.accounted_delta(), name="accepted - removed"),
            agg_line(mempool.unexplained_delta(), name="unexplained |delta|"),
            unit="locale", id=71, h=9, min=None,
            axis_label="messages / 5m",
            styles=[
                series_style("measured stock delta", color="blue", width=3),
                series_style("accepted - removed", color="green", dash=(6, 4), fill=0),
                series_style("unexplained |delta|", color="red", fill=0),
            ],
            description=(
                "Five-minute per-node reconciliation across all destination chains and priorities. "
                "All lines are message counts over the same five-minute endpoints: measured is the "
                "sum of delta(total state buckets); accounted is increase(new accepted insertions) "
                "minus the sum of increase(every removal reason); unexplained |delta| is their "
                "per-node absolute difference before node aggregation. Delta and increase both "
                "extrapolate range endpoints, so it should stay near zero apart from scrape "
                "alignment and restart or schema-rollout transients. "
                "Reprioritized admissions are "
                "deliberately excluded because moving an existing entry between priorities is "
                "stock-neutral. Only stateful exporters render: legacy accepted counters also "
                "counted successful validation on nodes that did not store the message, so showing "
                "old history here would invent growth. "
                "The Node aggregation switch chooses the pointwise worst, p95, median, or best "
                "selected node independently per line; worst therefore finds the largest residual "
                "regardless of its original sign. The underlying accepted-minus-removals "
                "identity is exact from process start on every upgraded process: it is informative "
                "on validators and collators that store messages and trivially 0 = 0 on relay-only "
                "nodes. These plotted window deltas remain estimates. A sustained residual means a "
                "missing transition counter or gauge-accounting bug."
            ),
        ),
        agg_timeseries(
            "External inclusion latency by node",
            agg_line(mempool.inclusion_quantile(0.5), name="p50"),
            agg_line(mempool.inclusion_quantile(0.95), name="p95"),
            unit="s", id=72, h=9,
            axis_label="seconds stored",
            description=(
                "How long an entry sat in a node's pool before that same node observed the message "
                "in an applied block. Each quantile is one node's own event quantile over a fixed "
                "five-minute window, and the Node aggregation switch then collapses across nodes. "
                "The clock starts at local admission and stops "
                "at local observation, so it contains broadcast propagation to whichever collator "
                "included the message plus this node's apply lag; it is a node-local reading, not a "
                "chain-wide inclusion time, and a slow or lagging node inflates its own value. "
                "Only applied removals are observed: expired, filtered, rejected_final and "
                "pool_pressure evictions are never sampled. A pool that is failing to get its "
                "messages included therefore draws FEWER samples rather than a higher latency, so "
                "read this beside the removal-reason and backlog panels above, which is where such "
                "a failure actually shows. Reprioritizing a duplicate replaces the entry and "
                "restarts its clock. Values are bounded by the 600s TTL by construction."
            ),
        ),
    ]),
]


def build():
    return dashboard(
        SELF, "TON Blockchain", ["ton", "blockchain"], VARIABLES, ROWS,
        refresh="10s", version=1,
        links=[nav_link(), {
            "title": "Session logs (devnet)",
            "type": "link",
            "icon": "external link",
            "targetBlank": True,
            "includeVars": False,
            "keepTime": False,
            "url": ("http://devnet-01.toncenter.com:8000/"
                    "?start=${__from:date:iso}&end=${__to:date:iso}"),
            "tooltip": ("Log-based per-run explorer for the devnet; opens with this dashboard's "
                        "time range."),
        }],
    )
