# TON Prometheus recording rules

`rules/ton-recording.yml` contains optional recording rules for deployments that want stable,
precomputed forms of common TON metric queries. The Grafana dashboards do not require these rules:
they query the base `ton_*` metrics directly and remain portable across Prometheus datasources.

The repository intentionally ships no Prometheus alerting rules. Operational triage remains visible
inside the TON Overview dashboard, where conditions are evaluated inline over the selected datasource
and time range. Alert routing, paging policy, and deployment-specific thresholds belong to the
deployment that owns Prometheus or Grafana alerting.

## Validate

Run Prometheus' rule checker and the rule unit tests after changing the recording rules:

```sh
promtool check rules metrics/prometheus/rules/ton-recording.yml
promtool test rules metrics/prometheus/tests/ton-recording.test.yml
```

The tests pin the reporter-population guards, which trust a rule's preferred numerator only while
both families come from exactly the same reporters. Those guards are two-sided set differences on
`(job, instance)`, not count comparisons: two equal-sized reporter populations can be disjoint,
which is exactly the shape a rolling upgrade produces, and one test case is that shape.

The rules file is datasource-agnostic and carries no dashboard variables. Node-level rules retain
`(job, instance)` so consumers can attribute a result; chain-level rules retain `job` so one
Prometheus can scrape multiple networks without merging them.

## Dashboard development

Dashboard queries and generated JSON live under `metrics/grafana`. Validate their semantic structure
and generated-file drift with:

```sh
python3 -m metrics.grafana.dashgen.build validate
python3 -m metrics.grafana.dashgen.build check
```
