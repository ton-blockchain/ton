import copy
import json
import unittest

from metrics.grafana.dashgen import (
    ton_actors,
    ton_blockchain,
    ton_network,
    ton_overview,
)
from metrics.grafana.dashgen.lib import (
    FLEET_SPREAD_STYLES,
    fleet_spread,
    summary_column,
    summary_table,
)
from metrics.grafana.dashgen.lib.core import validate_dashboard

BOARDS = (ton_actors, ton_blockchain, ton_network, ton_overview)

BLOCKCHAIN_GLANCE_PANELS = {
    60: "Collation summary (5m) — Masterchain",
    65: "Collation summary (5m) — Shardchain",
    61: "Validation summary (5m) — Masterchain",
    66: "Validation summary (5m) — Shardchain",
    62: "Collate time stats — mean per block — ${chain:text}",
    63: "Collate CPU stats — mean per block — ${chain:text}",
    64: "Slot timeline — zero is slot start — Masterchain",
    67: "Slot timeline — zero is slot start — Shardchain",
}
BLOCKCHAIN_CHAIN_PAIRED = ((60, "master"), (65, "shard"), (61, "master"), (66, "shard"),
                           (64, "master"), (67, "shard"))
# The collation-side path only: validation is owned by an identity that holds no slot zero, so it
# is not an exported mark and gets no line here.
SLOT_TIMELINE_LEGEND = ["Collation starts", "Collation finishes",
                        "Certificate finalizes", "Block applies", "slot start", "slot end"]
BLOCKCHAIN_DIAGNOSTIC_ROWS = (
    (104, "Chain health history"),
    (101, "Block flow & production outcomes"),
    (102, "Collation diagnostics"),
    (108, "Validation diagnostics"),
    (109, "Block workload & storage"),
    (106, "Consensus & finality"),
    (105, "Block propagation"),
    (103, "External message flow"),
    (110, "Mempool diagnostics"),
)
BLOCKCHAIN_DIAGNOSTIC_PANELS = {
    102: [8, 47, 49, 51, 53, 70],
    108: [56, 48, 50, 52, 54],
    109: [37, 23, 24, 38, 68, 69],
    106: [43, 41, 55, 42],
    105: [39, 40],
    103: [25, 26, 29, 10, 11],
    110: [27, 28, 30, 12, 71, 72],
}
BLOCKCHAIN_REMOVED_PANELS = {1, 2, 4, 7, 9, 20, 21, 22, 46, 57}


def panels(dashboard):
    pending = list(dashboard["panels"])
    while pending:
        item = pending.pop()
        yield item
        pending.extend(item.get("panels", []))


def expressions(panel):
    return " ".join(query["expr"] for query in panel.get("targets", []))


def legends(panel):
    return [query.get("legendFormat", "") for query in panel.get("targets", [])]


class DashboardGenerationTest(unittest.TestCase):
    def test_blockchain_default_view_is_the_chain_paired_glance_row(self):
        dashboard = ton_blockchain.build()
        visible = [panel for panel in dashboard["panels"] if panel["type"] != "row"]
        rows = [panel for panel in dashboard["panels"] if panel["type"] == "row"]

        self.assertLessEqual(len(visible), 8)
        self.assertEqual(
            [(panel["id"], panel["title"]) for panel in visible],
            list(BLOCKCHAIN_GLANCE_PANELS.items()),
        )
        self.assertEqual(rows[0]["id"], 107)
        self.assertEqual(rows[0]["title"], "Block production at a glance")
        self.assertFalse(rows[0]["collapsed"])
        self.assertEqual(
            [(row["id"], row["title"]) for row in rows[1:]],
            list(BLOCKCHAIN_DIAGNOSTIC_ROWS),
        )
        self.assertTrue(all(row["collapsed"] for row in rows[1:]))

    def test_blockchain_diagnostics_are_small_and_have_one_subject_per_row(self):
        dashboard = ton_blockchain.build()
        rows = {panel["id"]: panel for panel in dashboard["panels"]
                if panel["type"] == "row"}

        for row_id, item in rows.items():
            if row_id == 107:
                continue
            with self.subTest(row=row_id):
                limit = 6 if row_id in (102, 104, 109, 110) else 5
                self.assertLessEqual(len(item["panels"]), limit)
        for row_id, panel_ids in BLOCKCHAIN_DIAGNOSTIC_PANELS.items():
            with self.subTest(row=row_id):
                self.assertEqual([panel["id"] for panel in rows[row_id]["panels"]], panel_ids)

    def test_blockchain_redundant_chain_tiles_and_old_diagnostics_are_gone(self):
        dashboard = ton_blockchain.build()
        all_panels = list(panels(dashboard))
        ids = {panel["id"] for panel in all_panels}

        self.assertNotIn(100, ids)
        self.assertTrue(BLOCKCHAIN_REMOVED_PANELS.isdisjoint(ids))

    def test_mempool_surfaces_use_stateful_stock_and_honest_flow_accounting(self):
        overview = {panel["id"]: panel for panel in panels(ton_overview.build())}
        blockchain = {panel["id"]: panel for panel in panels(ton_blockchain.build())}

        for panel in (overview[9], blockchain[27]):
            with self.subTest(panel=panel["title"]):
                query_text = expressions(panel)
                self.assertIn('state="eligible"', query_text)
                self.assertIn('state=""', query_text)
                self.assertIn("sum by (job, instance)", query_text)
                self.assertEqual(panel["fieldConfig"]["defaults"]["unit"], "locale")
                self.assertIn("all", panel["description"].lower())
                self.assertIn("priorit", panel["description"].lower())

        state = blockchain[12]
        drawn = [query for query in state["targets"]
                 if not query["refId"].startswith("ATTR")]
        self.assertEqual(
            [query["legendFormat"] for query in drawn],
            ["eligible backlog", "total stored", "expiry handler lag"],
        )
        self.assertIn('state=~"eligible|postponed"', drawn[1]["expr"])
        self.assertIn("sum by (job, instance)", drawn[1]["expr"])
        self.assertNotIn("max by (job, instance)", drawn[1]["expr"])
        self.assertIn("clamp_min(ton_mempool_oldest_ext_message_age_seconds", drawn[2]["expr"])
        self.assertIn(" - 600, 0)", drawn[2]["expr"])
        self.assertIn("and on (job, instance)", drawn[2]["expr"])
        self.assertIn('state="eligible"', drawn[2]["expr"])
        self.assertEqual(state["fieldConfig"]["defaults"]["unit"], "locale")
        expiry_style = next(
            override for override in state["fieldConfig"]["overrides"]
            if override["matcher"] == {
                "id": "byRegexp",
                "options": r"^expiry handler lag(?: ⇒ .*)?$",
            }
        )
        expiry_properties = {item["id"]: item["value"]
                             for item in expiry_style["properties"]}
        self.assertEqual(expiry_properties["unit"], "s")
        self.assertEqual(expiry_properties["custom.axisPlacement"], "right")
        self.assertIn("Node aggregation switch", state["description"])
        self.assertIn("active flag is true", state["description"])
        self.assertIn("retry deadline", state["description"])
        self.assertIn("legacy expiry semantics", state["description"])
        self.assertNotIn("Worst selected node at each point", state["description"])

        for oldest in (overview[29], blockchain[28]):
            with self.subTest(panel=oldest["title"]):
                query_text = expressions(oldest)
                self.assertIn("and on (job, instance)", query_text)
                self.assertIn('state="eligible"', query_text)
                self.assertIn("legacy nodes are omitted", oldest["description"].lower())

        reconciliation = blockchain[71]
        query_text = expressions(reconciliation)
        self.assertIn("delta(ton_mempool_ext_messages", query_text)
        self.assertIn("increase(ton_mempool_ext_admission_total", query_text)
        self.assertIn("increase(ton_mempool_ext_removed_total", query_text)
        self.assertNotIn("deriv(", query_text)
        self.assertNotIn("rate(", query_text)
        self.assertIn("abs(", query_text)
        self.assertIn('outcome="accepted"', query_text)
        self.assertIn("ton_mempool_ext_removed_total", query_text)
        self.assertNotIn('outcome=~"accepted|reprioritized"', query_text)
        self.assertNotIn('state=""', query_text)
        accounted = next(query for query in reconciliation["targets"]
                         if query.get("legendFormat") == "accepted - removed")
        self.assertIn("and on (job, instance)", accounted["expr"])
        self.assertIn('state="eligible"', accounted["expr"])
        self.assertIn("reprioritized", reconciliation["description"].lower())
        self.assertIn("stateful exporters", reconciliation["description"].lower())
        self.assertIn("unexplained |delta|", legends(reconciliation))
        self.assertEqual(reconciliation["fieldConfig"]["defaults"]["unit"], "locale")
        self.assertEqual(reconciliation["fieldConfig"]["defaults"]["custom"]["axisLabel"],
                         "messages / 5m")
        self.assertNotIn("min", reconciliation["fieldConfig"]["defaults"])

        successful = blockchain[25]
        self.assertIn('outcome=~"accepted|reprioritized|validated_only"', expressions(successful))
        outcomes = blockchain[11]
        self.assertIn('outcome!~"accepted|validated_only|duplicate|reprioritized"',
                      expressions(outcomes))
        overview_issues = overview[17]
        self.assertIn('outcome!~"accepted|validated_only|duplicate|reprioritized"',
                      expressions(overview_issues))
        self.assertIn('outcome!="duplicate"', expressions(overview_issues))

        removals = blockchain[30]
        self.assertEqual(removals["title"],
                         "Local mempool removals /s (5m, per reason) (${agg:text} node ▾)")
        self.assertIn("ton_mempool_ext_removed_total", expressions(removals))
        self.assertNotIn('reason!="applied"', expressions(removals))
        self.assertIn("Applied is normal cleanup", removals["description"])
        self.assertIn("Node aggregation switch", removals["description"])
        self.assertNotIn("Scope: worst selected node", removals["description"])

        inclusion = blockchain[72]
        self.assertEqual(inclusion["title"],
                         "External inclusion latency by node (${agg:text} node ▾)")
        query_text = expressions(inclusion)
        self.assertIn("histogram_quantile(0.5,", query_text)
        self.assertIn("histogram_quantile(0.95,", query_text)
        self.assertIn("sum by (job, instance, le) (rate(ton_mempool_ext_inclusion_seconds_bucket",
                      query_text)
        self.assertEqual(
            [query["legendFormat"] for query in inclusion["targets"]
             if not query["refId"].startswith("ATTR")],
            ["p50", "p95"],
        )
        self.assertEqual(inclusion["fieldConfig"]["defaults"]["unit"], "s")
        self.assertIn("FEWER samples", inclusion["description"])
        self.assertIn("Reprioritizing a duplicate", inclusion["description"])

    def test_blockchain_collapsed_diagnostics_open_directly_below_their_row(self):
        dashboard = ton_blockchain.build()
        rows = {panel["id"]: panel for panel in dashboard["panels"]
                if panel["type"] == "row"}

        for row_id, _ in BLOCKCHAIN_DIAGNOSTIC_ROWS:
            with self.subTest(row=row_id):
                item = rows[row_id]
                self.assertTrue(item["collapsed"])
                self.assertTrue(item["panels"])
                self.assertEqual(
                    item["panels"][0]["gridPos"]["y"],
                    item["gridPos"]["y"] + 1,
                )

    def test_blockchain_glance_summaries_are_compact_event_distributions(self):
        built = {panel["id"]: panel for panel in panels(ton_blockchain.build())}

        for panel_id, operation, chain in ((60, "collate", "master"), (65, "collate", "shard"),
                                           (61, "validate", "master"), (66, "validate", "shard")):
            with self.subTest(panel=panel_id):
                panel = built[panel_id]
                query_text = expressions(panel)
                self.assertEqual(panel["type"], "table")
                self.assertLessEqual(panel["gridPos"]["h"], 8)
                self.assertEqual(len(panel["targets"]), 3)
                self.assertTrue(all(query["instant"] and query.get("format") == "table"
                                    for query in panel["targets"]))
                self.assertIn(f'chain="{chain}"', query_text)
                self.assertNotIn("${chain", query_text)
                self.assertIn(f'operation="{operation}"', query_text)
                self.assertIn('result="ok"', query_text)
                self.assertIn('clock=~"elapsed|real|cpu"', query_text)
                self.assertIn("histogram_quantile(0.5", query_text)
                self.assertIn("histogram_quantile(0.95", query_text)
                self.assertIn("ton_block_processing_duration_recent_max_seconds", query_text)
                self.assertIn("topk by (clock) (1", query_text)
                self.assertIn("job, instance", query_text)
                self.assertIn('job=~"$job"', query_text)
                self.assertNotIn("$instance", query_text)
                self.assertEqual(
                    list(panel["transformations"][-1]["options"]["indexByName"]),
                    ["clock", "Value #A", "Value #B", "Value #C", "instance"],
                )

    def test_summary_table_field_order_uses_display_names_but_emits_source_names(self):
        panel = summary_table(
            "Ordered summary",
            "stage",
            [summary_column("p95", "vector(1)")],
            id=999,
            key_name="Stage",
            label_columns=(("instance", "owner"),),
            field_order=("Stage", "owner", "p95"),
            description="Test table ordering.",
        )

        self.assertEqual(
            list(panel["transformations"][-1]["options"]["indexByName"]),
            ["stage", "instance", "Value #A"],
        )

    def test_summary_table_rejects_incomplete_or_ambiguous_display_fields(self):
        base = {
            "title": "Invalid summary",
            "key_label": "stage",
            "columns": [summary_column("p95", "vector(1)")],
            "id": 999,
            "description": "Test validation.",
        }
        with self.assertRaisesRegex(ValueError, "every displayed field exactly once"):
            summary_table(**base, field_order=())
        with self.assertRaisesRegex(ValueError, "displayed field names must be unique"):
            summary_table(**base, label_columns=(("instance", "p95"),))

    def test_blockchain_glance_stacks_are_additive_outer_collation_work(self):
        built = {panel["id"]: panel for panel in panels(ton_blockchain.build())}
        forbidden_nested_phases = ("trx_tvm", "trx_storage_stat", "trx_other",
                                   "prelim_storage_stat")

        for panel_id, clock, residual in ((62, "real", "Other work"),
                                          (63, "cpu", "Other CPU")):
            with self.subTest(panel=panel_id):
                panel = built[panel_id]
                query_text = expressions(panel)
                self.assertEqual(
                    panel["fieldConfig"]["defaults"]["custom"]["stacking"]["mode"],
                    "normal",
                )
                self.assertIn('job=~"$job"', query_text)
                self.assertNotIn("$instance", query_text)
                self.assertIn('operation="collate"', query_text)
                self.assertIn(f'clock="{clock}"', query_text)
                self.assertIn('result="ok"', query_text)
                for phase in ton_blockchain.PIPELINE_ORDER:
                    self.assertIn(phase, query_text)
                for phase in forbidden_nested_phases:
                    self.assertNotIn(phase, query_text)
                self.assertIn(residual, legends(panel))

        real_panel = built[62]
        wait_queries = [query["expr"] for query in real_panel["targets"]
                        if "wait" in query.get("legendFormat", "").lower()]
        self.assertEqual(
            {legend for legend in legends(real_panel) if "wait" in legend.lower()},
            {"Wait externals", "Other wait"},
        )
        self.assertTrue(all("-1 *" in expr for expr in wait_queries))
        self.assertNotIn("wait_externals", expressions(built[63]))

    def test_blockchain_glance_timeline_is_slot_relative_and_reads_in_execution_order(self):
        built = {panel["id"]: panel for panel in panels(ton_blockchain.build())}

        for panel_id, chain in ((64, "master"), (67, "shard")):
            with self.subTest(panel=panel_id):
                panel = built[panel_id]
                query_text = expressions(panel)
                legend_text = " ".join(legends(panel)).lower()

                self.assertIn("ton_consensus_collator_slot_mark_seconds", query_text)
                self.assertIn(f'chain="{chain}"', query_text)
                self.assertNotIn("${chain", query_text)
                self.assertIn('job=~"$job"', query_text)
                self.assertNotIn("$instance", query_text)
                self.assertIn("vector(0)", query_text)
                self.assertIn("${slot_seconds:raw}", query_text)
                self.assertNotIn("recent_max", query_text)
                self.assertNotIn("p95", legend_text)
                self.assertNotIn("mean", legend_text)
                # Named in the order the marks happen, so no rename transform can reorder them.
                self.assertEqual(legends(panel), SLOT_TIMELINE_LEGEND)
                self.assertFalse(panel.get("transformations"))

    def test_blockchain_glance_pairs_ignore_the_chain_selector(self):
        built = {panel["id"]: panel for panel in panels(ton_blockchain.build())}

        for panel_id, chain in BLOCKCHAIN_CHAIN_PAIRED:
            with self.subTest(panel=panel_id):
                description = built[panel_id]["description"]
                self.assertIn(f"pinned to chain={chain}", description)
                self.assertIn("ignores the Chain selector", description)
        for panel_id in (62, 63):
            self.assertIn("${chain:raw}", expressions(built[panel_id]))

    def test_every_board_shows_run_annotations_and_blockchain_links_to_the_explorer(self):
        for module in BOARDS:
            with self.subTest(board=module.__name__):
                annotations = module.build()["annotations"]["list"]
                self.assertEqual([item["name"] for item in annotations],
                                 ["Annotations & Alerts", "Runs"])
                self.assertEqual(annotations[1]["target"]["tags"], ["ton-run"])
                self.assertFalse(annotations[1]["hide"])

        explorer = ton_blockchain.build()["links"][-1]
        self.assertEqual(explorer["type"], "link")
        self.assertTrue(explorer["targetBlank"])
        self.assertIn("start=${__from:date:iso}&end=${__to:date:iso}", explorer["url"])

    def test_consensus_row_has_one_compact_owner_attributed_surface_per_question(self):
        dashboard = ton_blockchain.build()
        row = next(panel for panel in dashboard["panels"] if panel["id"] == 106)
        children = {panel["id"]: panel for panel in row["panels"]}

        self.assertEqual(list(children), [43, 41, 55, 42])
        for panel in children.values():
            self.assertIn('job=~"$job"', expressions(panel))
            self.assertNotIn("$instance", expressions(panel))
        for panel_id in (41, 55, 42):
            self.assertEqual(children[panel_id]["type"], "table")

        metric_owner = {
            "ton_consensus_collator_slot_mark_seconds": 41,
            "ton_consensus_collator_slot_mark_recent_max_seconds": 41,
            "ton_consensus_stage_seconds": 55,
            "ton_consensus_stage_recent_max_seconds": 55,
            "ton_consensus_slot_gap_seconds": 42,
            "ton_consensus_slot_lead_seconds": 42,
            "ton_consensus_slot_gap_recent_max_seconds": 42,
            "ton_consensus_slot_lead_recent_max_seconds": 42,
            "ton_consensus_rounds_total": 43,
        }
        for metric, owner in metric_owner.items():
            with self.subTest(metric=metric):
                self.assertEqual(
                    [panel_id for panel_id, panel in children.items()
                     if metric in expressions(panel)],
                    [owner],
                )

    def test_consensus_round_outcomes_are_fixed_window_per_group(self):
        panel = next(panel for panel in panels(ton_blockchain.build()) if panel["id"] == 43)
        query_text = expressions(panel)

        self.assertIn("ton_consensus_rounds_total", query_text)
        self.assertIn("ton_validator_groups", query_text)
        self.assertIn("[5m]", query_text)
        self.assertNotIn("$__rate_interval", query_text)
        self.assertIn(" / on (job, instance) group_left () ", query_text)
        self.assertIn("avg by (outcome)", query_text)

    def test_consensus_finality_summary_is_p95_and_recent_worst_for_final_marks(self):
        panel = next(panel for panel in panels(ton_blockchain.build()) if panel["id"] == 41)
        query_text = expressions(panel)

        self.assertIn('mark=~"finalize_cert|apply"', query_text)
        self.assertIn("histogram_quantile(0.95", query_text)
        self.assertNotIn("histogram_quantile(0.5", query_text)
        self.assertIn("ton_consensus_collator_slot_mark_recent_max_seconds", query_text)
        self.assertIn("job, instance", query_text)
        self.assertIn("max_owner", query_text)

    def test_consensus_stage_summary_keeps_tails_slow_mean_samples_and_two_owners(self):
        panel = next(panel for panel in panels(ton_blockchain.build()) if panel["id"] == 55)
        query_text = expressions(panel)

        self.assertEqual(len(panel["targets"]), 5)
        self.assertIn("histogram_quantile(0.5", query_text)
        self.assertIn("histogram_quantile(0.95", query_text)
        self.assertIn("ton_consensus_stage_seconds_sum", query_text)
        self.assertIn("ton_consensus_stage_seconds_count", query_text)
        self.assertIn("increase(ton_consensus_stage_seconds_count", query_text)
        self.assertIn("ton_consensus_stage_recent_max_seconds", query_text)
        self.assertIn("topk by (stage) (1", query_text)
        self.assertIn("slow_owner", query_text)
        self.assertIn("max_owner", query_text)

    def test_consensus_handoff_summary_keeps_both_sides_and_recent_owners(self):
        panel = next(panel for panel in panels(ton_blockchain.build()) if panel["id"] == 42)
        query_text = expressions(panel)

        self.assertEqual(len(panel["targets"]), 3)
        for side in ("slot_gap", "slot_lead"):
            self.assertIn(f"ton_consensus_{side}_seconds", query_text)
            self.assertIn(f"ton_consensus_{side}_recent_max_seconds", query_text)
        self.assertIn("histogram_quantile(0.5", query_text)
        self.assertIn("histogram_quantile(0.95", query_text)
        self.assertIn("job, instance", query_text)
        self.assertIn("max_owner", query_text)

    def test_fleet_spread_is_the_complete_attributed_house_shape(self):
        spread = fleet_spread("per_node_reading")
        self.assertEqual(
            [item.name for item in spread],
            ["worst (max)", "p95", "median", "average", "best (min)"],
        )
        self.assertEqual(spread[0].inner, "per_node_reading")
        self.assertEqual(spread[1].expr, "quantile(0.95, per_node_reading)")
        self.assertEqual(spread[2].expr, "quantile(0.5, per_node_reading)")
        self.assertEqual(spread[3].expr, "avg(per_node_reading)")
        self.assertEqual(spread[4].expr, "min(per_node_reading)")
        self.assertNotIn("super-light-blue", json.dumps(FLEET_SPREAD_STYLES))

    def test_build_is_pure_and_repeatable(self):
        for module in BOARDS:
            with self.subTest(board=module.__name__):
                rows = copy.deepcopy(module.ROWS)
                variables = copy.deepcopy(module.VARIABLES)
                first = module.build()
                second = module.build()
                self.assertEqual(first, second)
                self.assertEqual(rows, module.ROWS)
                self.assertEqual(variables, module.VARIABLES)

    def test_every_board_validates_and_has_no_alerts_query(self):
        for module in BOARDS:
            with self.subTest(board=module.__name__):
                dashboard = module.build()
                validate_dashboard(dashboard)
                self.assertNotIn('ALERTS{', json.dumps(dashboard))

    def test_node_timeline_warn_tier_survives_missing_warn_samples(self):
        # The tier sum is label-matched vector addition. A condition whose red arm covers nodes
        # its warn expression has no sample for — mc-stale firing because the age gauge vanished —
        # must not lose those nodes to the addition: the warn operand has to be anchored on the
        # red bool's labelset with `or 0 * (red)`.
        from metrics.grafana.dashgen.lib import condition
        from metrics.grafana.dashgen.lib.conventions import node_condition_rows

        conds = [
            condition("redonly", "red only", severity="critical", scope="node",
                      red="red_expr", for_s=60),
            condition("both", "both tiers", severity="warning", scope="node",
                      red="red_expr", warn="warn_expr", for_s=60),
        ]
        expr = node_condition_rows(conds)
        self.assertIn("clamp_max((red_expr) * 2 + ((warn_expr) or 0 * (red_expr)), 2)", expr)
        self.assertNotIn("+ (warn_expr)", expr.replace("+ ((warn_expr)", ""))
        # And the real board keeps the anchoring for every warn-carrying node condition.
        board_expr = next(
            query["expr"] for panel in panels(ton_overview.build())
            for query in panel.get("targets", [])
            if panel.get("title", "").startswith("Nodes that went bad"))
        warn_conditions = [c for c in ton_overview.TRIAGE_CONDITIONS
                           if c.scope == "node" and c.warn]
        self.assertTrue(warn_conditions)
        # The union appears twice in the final expression — once as the live vector and once
        # inside the range-activity filter — so each anchoring shows up exactly twice.
        self.assertEqual(board_expr.count(") or 0 * ("), 2 * len(warn_conditions))

    def test_attribution_owner_is_anchored_to_range_end(self):
        # A derived overlay picks one owner for the visible range, so it must be pinned to the
        # range end. A decomposition overlay (explicit `overlay=`) has no owner to pin: its rows
        # are per-node contributions guarded to nonzero, recognizable by the shared-denominator
        # join. Every overlay must be one or the other.
        for module in BOARDS:
            with self.subTest(board=module.__name__):
                overlays = [query for panel in panels(module.build())
                            for query in panel.get("targets", [])
                            if query["refId"].startswith("ATTR")]
                self.assertTrue(overlays)
                for query in overlays:
                    expr = query["expr"]
                    anchored = "@ end()" in expr
                    decomposed = "/ on () group_left ()" in expr and expr.rstrip().endswith("> 0")
                    self.assertTrue(anchored or decomposed, expr)

    def test_job_scoped_panels_do_not_read_instance(self):
        for module in BOARDS:
            for panel in panels({"panels": module.ROWS}):
                if panel.get("_queryScope") != "job":
                    continue
                with self.subTest(board=module.__name__, panel=panel["id"]):
                    self.assertNotIn("$instance", expressions(panel))

    def test_block_tail_panels_use_fixed_window_and_never_stack_quantiles(self):
        found = []
        for panel in panels(ton_blockchain.build()):
            queries = [query["expr"] for query in panel.get("targets", [])
                       if "histogram_quantile" in query["expr"]]
            if not queries:
                continue
            found.extend(queries)
            with self.subTest(panel=panel["id"]):
                self.assertTrue(all("[5m]" in expr for expr in queries))
                self.assertTrue(all("$__rate_interval" not in expr for expr in queries))
                if panel["type"] == "timeseries":
                    self.assertEqual(
                        panel["fieldConfig"]["defaults"]["custom"]["stacking"]["mode"],
                        "none",
                    )
        self.assertTrue(found)

    def test_processing_tails_cover_cpu_and_exact_attributed_maxima(self):
        all_panels = list(panels(ton_blockchain.build()))
        built = {panel["id"]: panel for panel in all_panels}
        tails = [panel for panel in all_panels
                 if "ton_block_processing_duration_seconds_bucket" in expressions(panel)]
        self.assertEqual(
            {operation for panel in tails for operation in ("collate", "validate")
             if f'operation="{operation}"' in expressions(panel)},
            {"collate", "validate"},
        )
        for panel in tails:
            query_text = expressions(panel)
            self.assertIn("cpu", query_text)
            self.assertIn('result="ok"', query_text)
            self.assertNotIn('result="error"', query_text)

        for panel_id in (51, 52):
            with self.subTest(panel=panel_id):
                query_text = expressions(built[panel_id])
                refs = {query["refId"] for query in built[panel_id]["targets"]}
                self.assertIn('result="error"', query_text)
                self.assertNotIn('result="ok"', query_text)
                self.assertTrue(any(ref.startswith("ATTR") for ref in refs))

        phase_maxima = [panel for panel in all_panels
                        if "ton_block_processing_phase_recent_max_seconds" in expressions(panel)]
        self.assertEqual({panel["id"] for panel in phase_maxima}, {53, 54})
        for panel in phase_maxima:
            self.assertIn('result=~"ok|error"', expressions(panel))
            self.assertTrue(all("{{result}}" in query.get("legendFormat", "")
                                for query in panel["targets"]))

    def test_triage_rate_thresholds_use_fixed_windows(self):
        built = {panel["id"]: panel for panel in panels(ton_overview.build())}
        for panel_id in (39, 43, 44, 45):
            with self.subTest(panel=panel_id):
                expressions = [query["expr"] for query in built[panel_id]["targets"]
                               if "ton_actor_worker_busy_ticks_total" in query["expr"]
                               or "wire_dropped_total" in query["expr"]]
                self.assertTrue(expressions)
                self.assertTrue(all("[5m]" in expr for expr in expressions))
                self.assertTrue(all("$__rate_interval" not in expr for expr in expressions))

    def test_failure_percentiles_keep_healthy_active_nodes(self):
        panel = next(panel for panel in panels(ton_overview.build()) if panel["id"] == 31)
        drawn = [query for query in panel["targets"] if not query["refId"].startswith("ATTR")]
        self.assertTrue(drawn)
        self.assertTrue(all(" or 0 * " in query["expr"] for query in drawn))

    def test_job_scope_rejects_instance_filter(self):
        dashboard = ton_actors.build()
        item = next(panel for panel in panels(dashboard) if panel.get("targets"))
        item["_queryScope"] = "job"
        item["targets"][0]["expr"] = 'up{instance=~"$instance"}'
        with self.assertRaisesRegex(ValueError, "narrowed by \\$instance"):
            validate_dashboard(dashboard)

    def test_duplicate_ref_id_is_rejected(self):
        dashboard = ton_blockchain.build()
        item = next(panel for panel in panels(dashboard) if len(panel.get("targets", [])) > 1)
        item["targets"][1]["refId"] = item["targets"][0]["refId"]
        with self.assertRaisesRegex(ValueError, "duplicate refId"):
            validate_dashboard(dashboard)

    def test_malformed_legend_label_is_rejected(self):
        dashboard = ton_network.build()
        item = next(panel for panel in panels(dashboard) if panel.get("targets"))
        item["targets"][0]["legendFormat"] = "{{direction, state}}"
        with self.assertRaisesRegex(ValueError, "invalid label"):
            validate_dashboard(dashboard)

    def test_overlap_is_rejected(self):
        dashboard = ton_overview.build()
        visible = [panel for panel in dashboard["panels"] if panel["type"] != "row"]
        visible[1]["gridPos"] = copy.deepcopy(visible[0]["gridPos"])
        with self.assertRaisesRegex(ValueError, "overlaps"):
            validate_dashboard(dashboard)


if __name__ == "__main__":
    unittest.main()
