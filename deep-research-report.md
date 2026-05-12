# Compact research pack для усиления Codex/LLM-агента в TON Consensus Bug Bounty

## Резюме

Для текущей цели не нужна большая «универсальная база эксплойтов». Нужен компактный pack, который улучшает именно поиск и проверку гипотез в TON consensus-коде: `validator/consensus/`, с исключением `validator/consensus/null`, на reference implementation из testnet branch, и с обязательной собственной локальной репродукцией issue перед репортом. Это прямо соответствует условиям urlTON Consensus Challenge docsturn0search2: primary scope — `validator/consensus/`, `validator/consensus/null` исключён, reference implementation — testnet branch, а каждый valid report должен включать reproduction script или полный reproduction archive. citeturn13view0turn13view1

Главный переносимый урок из urla16z crypto articleturn11search0 не в том, чтобы «накормить агента всеми старыми хаками», а в том, чтобы превратить исторические кейсы в структурированные skills и жёстко закрыть future-data leakage. В изолированной среде без «ответов из будущего» baseline-агент показал лишь 10% успеха, а skill-guided агент — 70%; при этом a16z отдельно подчёркивают, что агент часто правильно определяет сам класс уязвимости, но ломается на execution, attacker model, profitability / impact и корректной PoC-репродукции. citeturn20view4turn20view1turn20view2

Методологически это очень хорошо стыкуется с urlKnowdit paper on arXivturn0search1: брать исторические human-authored reports, строить из них knowledge graph / pattern memory, а дальше использовать multi-agent loop «specification generation → harness synthesis → fuzz execution → finding reflection». Для TON это значит: не тащить в Codex ERC20/AMM-паттерны, а загрузить именно incidents и bug classes из consensus / validator / P2P / state sync / malformed message / liveness-history. citeturn19view1turn19view2turn19view3

Практический вывод для TON: лучший research pack — это компактный набор исторических incident lessons + TON-relevant patterns + 3–5 skills + строгие false-positive filters. Это повышает качество hypothesis generation, но не заменяет ваш текущий workflow `hypothesis → skeptic → validation plan → local/cloud-only reproduction → only then report`. Такой дисциплинированный подход важен и потому, что challenge принимает только issues, которые участник воспроизвёл сам. citeturn13view0turn20view4

## Что переносим из a16z и Knowdit в TON, а что — нет

Переносить стоит не DeFi-семантику как таковую, а саму **форму памяти**: incident analysis, pattern taxonomy, workflow design и explicit validation discipline. В a16z skill set состоял из анализа инцидентов, категоризации паттернов и пошагового audit workflow; именно это дало скачок от 10% к 70%, но не решило execution bottleneck. Для TON это означает: Codex должен получать не просто «ищи race condition», а конкретные сценарные рамки вроде «проверь binding peer→validator→session до использования message metadata», «ищи queue growth under withheld progress», «не считай signature check достаточным cheap reject, если дорогое состояние уже тронуто». citeturn20view4turn20view1

Также обязательно переносится **анти-утечка будущих данных**. В a16z агент начал подсматривать реальные attack tx через `txlist` и тем самым «решал экзамен с открытым ответом». Для TON аналог утечки — это доступ к более поздним issue/PR/postmortem/commit diff, где уже объяснён баг, или к любым future artifacts, которых не было в момент target code snapshot. Для Codex это надо формализовать как запрет: не использовать будущие фиксы, публичные отчёты, поздние тесты и внешние продовые артефакты как доказательство существования бага. citeturn20view4

Ещё один полезный урок a16z: sandbox-policy нужно обеспечивать инструментально, а не только инструкциями. В их эксперименте агент смог обойти ожидаемые ограничения через инструменты среды. Для вашего TON workflow это подкрепляет уже существующую дисциплину «no public nodes / no live network / cloud-only local validation»: ограничения должны быть baked into environment, а не надеяться только на prompt text. citeturn20view2

Наконец, для TON важно заменить «DeFi semantics» на **Simplex semantics**. В опубликованных urlSimplex docsturn0search13 TON consensus описан как leader-based, slot-based протокол с notarization/finalization quorums, skip-votes, standstill resolution и candidate resolution; именно такие сущности и должны стать центральными forensics-объектами в памяти Codex. Иначе агент будет продолжать переносить слишком общие «blockchain-y» гипотезы без учёта slot/window/session invariants. citeturn23view0

## Исторические инциденты и что из них должен запомнить Codex

Ниже — компактная таблица инцидентов, которые реально полезны для TON consensus research. Обозначения: **Source quality** — `A` official advisory/postmortem/release, `B` maintainer-authored public retrospective, `C` high-quality curated technical secondary source. **Repro** — `H` high public reproducibility, `M` medium (точные epochs/heights/root cause, но без готового PoC-harness), `L` low (в основном постмортем без ready harness).

| Инцидент | Дата | Класс / компонент | Root cause + trigger + attacker model | Impact | Repro / patch | Релевантность для TON / чему учить Codex | Source |
|---|---:|---|---|---|---|---|---|
| Geth minority split via crafted transaction | 2021 | Consensus divergence / EVM execution path | Внутренний RETURNDATA/datacopy memory-corruption bug давал разный `stateRoot` на maliciously crafted transaction; attacker model — любой, кто может доставить crafted tx в canonic chain path | Minority chain split | H; official postmortem прямо говорит о public state test и патче `always copying data on input to precompiles` | **Очень высокая.** Учить Codex искать «malformed-but-valid-looking input reaches consensus state transformation» и любые panic/divergence paths до cheap rejection | A citeturn24view0turn24view3 |
| Ethereum beacon old-target attestation resource exhaustion (Prysm mainnet) | 2023 / 2025 | Liveness / attestation validation / state replay | Valid attestations with old target/head forced expensive beacon-state regeneration; cache exhaustion and repeated/concurrent replays amplified CPU/goroutine load; trigger — protocol-valid but adversarially bad attestation mix, often from out-of-sync peers/clients | Delayed finality, missed blocks, inactivity penalties, resource exhaustion | M; exact epochs, chain ranges, trigger examples and fix logic public; long-term fix — validate using head state, avoid prior state regeneration | **Очень высокая.** Учить Codex считать protocol-valid input adversarial if it triggers repeated expensive recomputation; signatures/spec-validity ≠ cheapness | A citeturn28view0turn28view1turn28view2 |
| Bitcoin Core CVE-2018-17144 | 2018 | Block validation / UTXO accounting / assert-vs-reject | Optimization removed duplicate-input check on newly received blocks; malformed block path hit assertion crash, and later rewrite created inflation variant; attacker model — malicious miner / block producer | Crash DoS; potential inflation / invalid acceptance | H; full disclosure, public patch/testcase history, exploited on testnet later | **Очень высокая.** Учить Codex искать removed/reordered checks, assert/panic in consensus path, and cases where malformed data becomes safety issue | A/B citeturn21view2turn22view0 |
| Bitcoin Core CVE-2012-2459 | 2012 | Invalidity-cache / duplicate-handling / partitioning | Invalid block could be mutated from a valid one while keeping same merkle root; vulnerable nodes cached rejection and later rejected valid version; attacker model — remote network attacker | Node isolation, forks / partitions | M; official alert withholds technical detail, curated technical secondary explains root cause | **Высокая.** Учить Codex искать replay/duplicate/invalidity cache poisoning and “same digest, different semantic validity” hazards | A/C citeturn21view3turn22view1 |
| Tendermint Mulberry | 2021 | Evidence handling / consensus reactor / state-machine ordering | Validators could verify/propose evidence for still in-flight blocks; evidence timestamp depended on last commit of non-finalized block; attacker model — network conditions + malicious behavior interacting with edge ordering | DoS / possible halt | M; detailed retrospective, versions and patch releases public | **Очень высокая.** Учить Codex искать use of unfinalized / in-flight state when forming consensus artifacts, especially evidence/cert objects | B citeturn13view8 |
| Tendermint Syringa | 2020 | Commit signature binding / full-node verification | Proposers could include signatures for wrong block; full nodes also stopped verifying all commit signatures after +2/3; attacker model — malicious validator, plus one variant exposed by chain-id reuse misconfig | Invalid proposals, indefinite halt, arbitrary commit data / false witness | M; retrospective and remediation public | **Очень высокая.** Учить Codex full binding of signatures to block/round/session and full-node/full-verifier behavior differences | B citeturn27view0turn27view2 |
| CometBFT ASA-2024-001 | 2024 | Governance param transition / vote extension enable height | Validation logic for `VoteExtensionsEnableHeight` could panic during governance-triggered parameter change; attacker model — governance-triggered but protocol-reachable transition | Chain halt | H/M; official advisory, patched in 0.38.3 | **Высокая.** Учить Codex искать state-machine edge bugs around config/session/epoch transitions, not only message parsing | A citeturn18view0 |
| CometBFT ASA-2024-011 | 2024 | Vote extension / sender-index validation ordering | Precommit path with non-`nil` `BlockID` handled vote extension before ordinary `ValidatorIndex` verification; invalid index then panicked receiver; attacker model — Byzantine validator/full node with malicious code; default networks w/o vote extensions unaffected | Crash / network halt | H/M; official advisory with exact invalid field and fix release | **Высокая.** Учить Codex: never touch extension / deeper logic before binding sender identity to current validator set | A citeturn18view1 |
| CometBFT ASA-2025-002 | 2025 | Gossip / block parts / proof-index binding | `Part.Index` was not required to equal `Part.Proof.Index`; node could accept and relay wrong part, mark correct part as already received, and stall network; attacker model — malicious peer disseminating seemingly valid parts | Network halt / dissemination poisoning | H/M; advisory includes technical deep-dive and patched versions | **Очень высокая.** Учить Codex проверять binding proof↔index↔payload before relay, cache mark, or suppression of future retries | A citeturn18view2 |
| CometBFT ASA-2024-009 | 2024 | State sync / validator-set bootstrap / proposer selection | State sync validated validator set but not `ProposerPriority`; malicious/invalid RPC state made node disagree about proposer selection and reject/propose wrong blocks; attacker model — malicious or custom RPC/bootstrap source | Chain split / eventual halt if enough validators sync from bad sources | H/M; official advisory, patched versions and exact missing validation published | **Очень высокая.** Учить Codex, что bootstrap/state-sync metadata is consensus surface, not “just ops.” Stale or partially validated membership state is dangerous | A citeturn25view1turn25view2 |
| Solana Mainnet Beta stall | 2020 | Block propagation / repair / duplicate-slot handling | A validator ran two instances and propagated different blocks for same slot; known repair-path bug could not reconcile duplicate blocks for same slot across partitions; attacker model — faulty or Byzantine validator/leader | Liveness failure / network stall | L/M; official postmortem, but no ready local harness | **Высокая.** Учить Codex проверять same-slot duplicate handling, cross-partition recovery, and “same slot vs different slot” repair assumptions | A citeturn17view0turn17view1 |
| Solana 9-14 outage | 2021 | Queueing / memory / fork explosion | Transaction flood filled forwarder queue without bound; resource-heavy blocks plus unbounded queue growth caused forks, OOM, crashes, restart lag; attacker model — transaction flood / protocol-valid load spike | Liveness failure, validator crashes, forced coordinated restart | L/M; official overview and mitigation summary public | **Очень высокая.** Учить Codex искать queue growth without hard cap/backpressure and combined effects of “valid but heavy” inputs with restart/fork behavior | A citeturn17view3turn17view4 |
| Polkadot postmortem | 2021 | Runtime resource bound + determinism drift | On-chain validator election overflowed Wasm memory budget; later native/Wasm compiler-version mismatch created `storage root mismatch`; attacker model — worst-case protocol path + build/runtime mismatch, not simple external malformed packet | Runtime failure, downtime, determinism break | M; exact blocks, fixes and mitigations public | **Высокая.** Учить Codex двум вещам: worst-case consensus computation must stay within budget, а deterministic assumptions across execution paths/builds are part of security | B citeturn17view5 |
| Lighthouse duplicate-attestation amplification | 2020 | Gossip duplicate handling / fork-choice interaction | Fork-choice bug plus duplicate suppression over only last 256 attestations led to cyclic amplification of duplicate attestations over gossipsub, overloading nodes; attacker model — buggy/forked network conditions amplified by propagation logic | Flooding / overload | M; maintainer blog explains root cause and fix direction | **Высокая.** Учить Codex, что duplicate suppression policy must be evaluated under forked/competing-head conditions, not just healthy network assumptions | B citeturn13view16 |

Из этой таблицы Codex должен вынести шесть базовых lessons. Во-первых, **protocol-valid inputs бывают ливнес-опаснее откровенно malformed inputs**: старые attestations, конфликтующие same-slot blocks или “почти валидные” block parts могут ломать систему не потому, что они проходят подписи, а потому что они запускают дорогую ветку обработки. citeturn28view0turn17view1turn18view2

Во-вторых, **identity/binding bugs — это first-class consensus bugs**. Валидация `ValidatorIndex`, соответствия proof index, proposer-selection state, commit signatures и других identity-carrying полей должна происходить _до_ любой углублённой обработки, relay side effects, queue marking или extension logic. citeturn18view1turn18view2turn25view2turn27view0

В-третьих, **duplicate/replay handling — не “сетевой шум”, а возможный корень partition/halt**. Bitcoin invalidity-cache bug, Solana same-slot duplicates и Lighthouse attestation amplification показывают, что “мы уже это видели” и “этот digest уже помечен” — очень опасные места, если нормальная и дефектная версии одного объекта путаются или suppression слишком грубый. citeturn22view1turn17view1turn13view16

В-четвёртых, **bootstrap и state sync входят в consensus attack surface**. Если нода принимает partial/insufficiently validated bootstrap state про validator set, proposer schedule или session metadata, это не ops-проблема, а консенсусная проблема. citeturn25view2turn17view5

В-пятых, **worst-case testing must include stalled progress / non-finality**, а не только здоровую сеть. Подобные pathologies часто не проявляются на обычных testnets; maintainer analysis вокруг Holesky прямо говорит, что нужны adverse-condition/non-finality exercises и механизмы восстановления, иначе клиенты плохо переживают реальный worst case. citeturn13view15turn28view0

В-шестых, **panic/assert в consensus path — это не «просто crash»**. Для blockchain clients такие места часто эквивалентны network halt, partition или safety-risk, особенно если input может быть сгенерирован Byzantine peer/validator или malformed-but-parser-accepted object. citeturn21view2turn18view1turn18view0

## TON-релевантные паттерны, которые стоит хранить в памяти Codex

Ниже — 12 reusable patterns, которые лучше всего переносятся в TON consensus research. Каждый паттерн сформулирован так, чтобы агент мог решить: это лишь слабая гипотеза или уже достойный validation target.

**Unsafe trust in P2P callback metadata.** Если код использует `sender`, `peer_id`, `validator_index`, `proof index` или другой transport-origin field до того, как он связан с текущим validator/session state, это high-value surface. Code smells: ранняя индексация по массиву валидаторов, `unwrap/assert`, relay/queue side effects до membership binding. Attacker model: malicious peer или Byzantine validator с валидным transport foothold. False positive: путь уже доминируется authenticated-overlay membership и unknown/stale senders дропаются до consensus logic. Evidence: нужен конкретный reachable path от raw message metadata к state mutation/queue insert/panic-prone branch без current-set check. Safe local validation: forged sender/index in local harness, no live network. В TON это надо применять к overlay/broadcast handlers, session transitions и любым местам, где peer identity проецируется на validator role. citeturn18view1turn18view2turn25view2

**Stale validator-set / stale peer mapping.** Даже если identity формально аутентифицирован, stale mapping может ломать proposer schedule, membership gates, quorum interpretation или peer-role assumptions. Smells: caches keyed only by peer/session ID without explicit epoch/session invalidation; membership map rebuilt lazily; bootstrap logic that validates validator addresses but not all schedule/binding fields. Attacker model: stale or malicious bootstrap source, Byzantine validator around session rotation, or out-of-date peer map. False positive: all role-bearing state is rebuilt atomically on session change and every message is checked against current epoch/session. Evidence: нужно показать, что stale mapped state реально влияет на consensus path, а не только metric/log path. Safe validation: local reconfiguration harness with validator-set/session change plus old message replay. Для TON это одна из самых ценных проверок, потому что Simplex explicitly depends on slot/window leader selection, notarization/finalization quorums and skip logic. citeturn25view2turn18view0turn23view0

**Malformed-but-valid-looking message crash.** Это класс, где объект проходит enough parsing/shape checks, чтобы попасть глубже в consensus machinery, а потом вызывает panic/assert/divergence. Smells: asserts in validation code, parser success before semantic binding, copy/aliasing assumptions, extension logic before core checks. Attacker model: miner, block producer, Byzantine validator or malicious peer who can inject crafted but syntactically acceptable object. False positive: impossible path after strong early semantic rejection and crash not reachable from remote input. Evidence: exact input class and exact crash/divergence point. Safe validation: parser fuzzing or unit reproduction with minimized crafted object. Для TON это значит не останавливаться на “deserialize failed/ok”, а искать, что происходит после частичного принятия candidate/vote/certificate. citeturn24view0turn21view2turn18view1

**Signature / quorum / certificate binding issue.** Если сигнатуры, votes, block parts или certificate fields не связаны полностью с `height/slot/round/block/session/validator`, появляется окно для halt, arbitrary data inclusion או false quorum semantics. Smells: partial verification after +2/3, “first enough signatures wins”, lack of proof-index equality, proposer-schedule fields not part of validation. Attacker model: Byzantine validator or malicious peer that can inject partially plausible artifacts. False positive: code verifies all signatures and all binding fields before any certificate is accepted. Evidence: показать missing binding field и downstream effect on safety/liveness/reward logic. Safe validation: local multi-node simulation with inconsistent bound fields and deterministic trace comparison. В TON это надо применять к votes/certificates, slot-parent references, validator membership, and any session-bound signing context. citeturn27view0turn18view2turn25view2

**Replay / duplicate handling gap.** Duplicates опасны не только как bandwidth noise: они могут poison caches, suppress correct retries, amplify themselves under fork conditions или разделить “invalid version” и “valid version” одного logical object. Smells: invalidity caches keyed too coarsely, dedup by digest without semantic dimension, same-slot suppression rules, “already received” bits set before full validation. Attacker model: malicious peer/validator or adversarial network schedule. False positive: dedup key includes all semantics that matter and state is only marked after full proof/binding checks. Evidence: нужен path, где duplicate or mutated version blocks later acceptance of correct object or causes network amplification. Safe validation: deterministic replay harness with valid/invalid twin objects and partitioned delivery. В TON особенно важно проверять same-slot / same-seqno / same-candidate-ID behaviors. citeturn22view1turn17view1turn13view16

**Resource exhaustion before cheap rejection.** Некоторые worst-case inputs валидны по протоколу, но экстремально дороги для валидации, и система тратит ресурс раньше, чем может safely reject/deprioritize them. Smells: state regeneration, expensive retries, deep verification before cheap structural filters, repeated replays without memoization. Attacker model: Byzantine validator or peer flooding valid-but-bad-cost objects. False positive: strong up-front bounds, memoization, backpressure and proof that input population is strictly capped. Evidence: exact expensive code path, its trigger volume, and why upstream guards do not bound it enough. Safe validation: local load harness using protocol-valid expensive messages plus withheld-progress or out-of-sync scenarios. В TON это прямо применимо к candidate resolution, validation waiters, certificate accumulation and any parent/state lookup path. citeturn28view0turn28view2turn17view3turn17view5

**Queue growth / missing backpressure.** Очередь/кэш/awaiter list без hard cap, deadline or drop policy часто превращает otherwise recoverable liveness glitch в OOM or permanent lag. Smells: unbounded vectors/maps of pending work, “requeue until advancement” loops, retry fan-out without per-key dedup, duplicate gossip amplification. Attacker model: heavy valid traffic, Byzantine peers, or withheld progress. False positive: strict per-slot/per-peer/window cap, bounded queue lengths, cancellation on advancement/teardown. Evidence: show enqueue path, blocked drain condition, absence of local caps, and inability of upstream bounds to keep pending work small. Safe validation: deterministic harness with withheld progress and repeated valid inputs, measuring unresolved tasks or pending entries. Для TON это один из самых конкурентоспособных паттернов в `validator/consensus/`, особенно там, где progress event is external to the waiting logic. citeturn17view3turn13view16turn28view2

**Byzantine-but-protocol-valid worst-case behavior.** Исторически многие инциденты вызывались не blatant-invalid objects, а корректными по спецификации сообщениями в плохом времени/объёме/сочетании: old-target attestations, same-slot duplicates, conflicting yet individually admissible artifacts. Smells: code assumes honest scheduling, healthy execution client, or “normal” arrival distribution. False positive: proof that protocol-valid worst case is bounded by design and tested under stressed conditions. Evidence: demonstrate that all checks pass, yet aggregate behavior still harms liveness/resources. Safe validation: produce local worst-case but spec-valid sequences, not malformed fuzz only. Для TON это означает, что “сообщение подписано и в правильном формате” — не конец проверки, а только начало attacker-model analysis. citeturn28view0turn17view1turn13view8

**State-machine edge ordering bug.** Ошибка может жить не в steady-state, а в границе между состояниями: governance param change, session rollover, evidence from in-flight blocks, skip/finalize race, start-up/bootstrap transition. Smells: code that computes one artifact using another object that is not yet finalized/committed/current, or uses old invariants during transition. False positive: transition fenced by explicit state-version checks and rollback-safe ordering. Evidence: exact state transition and the object whose semantics change across that edge. Safe validation: narrow integration test built around epoch/session/config transition. Для TON это особенно полезно из-за slot/window rules, skip votes, finalization and standstill/candidate-resolution mechanics in Simplex. citeturn13view8turn18view0turn23view0

**Timeout / cancellation / async-completion bug.** Здесь evidence quality обычно ниже, потому что это чаще maintainer postmortem theme, чем отдельная CVE. Но паттерн реальный: concurrent replays, task storms, poor prioritization and bad cleanup under non-finality repeatedly appear in client incidents. Smells: pending tasks survive advancement, same work can start many times concurrently, timeout path and success path both mutate shared state, teardown does not drain waiters. False positive: pending work is keyed/idempotent, advancement cancels or fulfills all dependents, and deterministic scheduler tests cover reordering. Evidence: exact pending-work lifecycle and at least one local schedule showing leaked or duplicated work. Safe validation: actor/scheduler tests with injected delays, retries and advancement/teardown races. В TON этот паттерн особенно важен для actor-style consensus code. citeturn28view2turn13view15

**Cross-slot / cross-window invariant break.** В slot-based protocols критично, чтобы “same slot”, “later slot”, “same window”, “different window”, “skipped”, “notarized”, “finalized” were never accidentally conflated. Smells: dedup keyed only by slot or only by hash, parent references accepted without window/slot relation checks, repairs that assume duplicates across slots behave like duplicates within a slot. False positive: invariants are explicit and exhaustively checked before state mutation. Evidence: show two artifacts that are individually plausible but violate global slot/window invariant when combined. Safe validation: local multi-node schedule with inserted duplicate/proposed/skip artifacts across adjacent slots/windows. Для TON это очень прямое применением because Simplex explicitly encodes leadership windows, skip votes and slot clearing rules. citeturn17view1turn23view0

**Bootstrap / state-sync trust gap and determinism drift.** Даже если сеть уже безопасна, новая или recovering node может стартовать из partial truth: stale proposer state, inconsistent runtime/compiler path, invalid locally reconstructed state. Smells: bootstrap acceptance of partially validated consensus metadata, native/Wasm or fast/slow path divergence, “should be deterministic” assumptions not enforced. False positive: identical validation semantics across all execution modes and full validation of bootstrap metadata. Evidence: show what field/mode is trusted but not checked, and why it affects later consensus decisions. Safe validation: two-node local sync/bootstrap harness or dual-execution comparison under same input. В TON это относится ко всем путям, где нода возобновляет/догоняет consensus state из external or persisted artifacts. citeturn25view2turn17view5turn24view0

## Codex skills и TON-specific false-positive filters

Ниже — пять skills, которые стоит прямо добавить в repo как markdown skills для Codex. Их задача — не генерировать громкие conjectures, а заставлять агента либо добыть точное evidence, либо рано отступить. Такой стиль хорошо соответствует both a16z’s “skills + leakage control + execution discipline” и challenge requirement “no report without self-reproduction.” citeturn20view4turn13view0

`p2p-sender-contract-review` — запускать на overlay / bus / broadcast handlers и на любом месте, где transport-origin metadata превращается в validator identity or role. Inputs: handler file, sender-related fields, current validator/session state. Procedure: trace peer→validator/session binding; verify that current membership check precedes queue insert, certificate accumulation, advanced parsing and panic-prone indexing; check stale-session replay paths; require explicit attacker model. Stop if authenticated membership dominates every path. Output: `binding point`, `first dangerous side effect`, `kill condition`, `local harness idea`. Основание: CometBFT incidents repeatedly showed that missing or delayed binding of sender-related metadata causes panic, halt or split. citeturn18view1turn18view2turn25view2

`malformed-message-crash-review` — запускать на parsers, vote/certificate decoders, block-part handlers и any “shape valid, semantics later” path. Inputs: deserializer, semantic checks, panic/assert sites. Procedure: map cheap reject ordering; find assert/panic/divergence after partial acceptance; look for aliasing/copy assumptions, proof-index mismatches and extension logic before core validation; design only local fuzz/harness, no live traffic. Stop if all remote-controlled error paths gracefully return before deep state touch. Output: `reachable crash?`, `exact malformed class`, `semantic gates`, `reproducer sketch`. Основание: Geth, Bitcoin Core and CometBFT all had exactly this class. citeturn24view0turn21view2turn18view1

`consensus-liveness-resource-review` — запускать на queues, retry loops, caches, waiters, parent/state resolution, attestation/candidate validation and any path gated by external progress. Inputs: pending-work structures, advancement signals, per-slot/per-peer bounds. Procedure: ask “can protocol-valid but bad-cost inputs force repeated expensive work?”; then “is pending work locally capped / cancelled / deduped?”; then “what progress event drains it and can that be withheld?”; then design a local withheld-progress harness. Stop if strict local bound exists and cannot scale with adversarial valid inputs. Output: `resource surface`, `worst-case trigger`, `bound proof or missing bound`, `safe measurement plan`. Основание: Prysm, Solana, Lighthouse and Polkadot incidents all cluster here. citeturn28view0turn17view3turn13view16turn17view5

`certificate-quorum-binding-review` — запускать на votes, certificates, proof parts, finality/notarization objects, state-sync validator sets and proposer schedules. Inputs: all signed fields, all non-signed helper fields, all indices/rounds/heights/slots/sessions used downstream. Procedure: enumerate what must be bound; check whether verification covers each field before object is trusted; compare full-node behavior vs fast path / bootstrap path; attempt mismatch scenarios locally. Stop if every helper field is either derived from signed data or separately validated before use. Output: `missing binding?`, `affected transition`, `attacker model`, `local scenario`. Основание: Tendermint Syringa, CometBFT block-part, vote-extension and state-sync advisories. citeturn27view0turn18view2turn25view2

`async-timeout-race-review` — запускать на actor-style code, timers, retry tasks, progress callbacks, teardown paths and “wait until X advances” logic. Inputs: pending promises/futures, cancellation points, advancement/teardown handlers. Procedure: map task lifecycle; ask what resolves/cancels pending work; test schedule permutations where timeout, success and teardown race; verify idempotence and drain behavior. Stop if lifecycle is key-deduped and teardown/advancement provably drains all dependents. Output: `pending-work graph`, `race window`, `safety vs liveness effect`, `deterministic local test sketch`. Основание: the postmortem literature repeatedly points to concurrent replays, scheduling, state-cache heuristics and non-finality recovery as failure zones. citeturn28view2turn13view15

TON-specific false-positive filters стоит держать отдельно и применять _до_ серьёзного validation effort. Исторические incidents показывают, что weak hypotheses часто выглядят убедительно ровно до того момента, когда проверяешь binding, slot/window caps или local reproducibility discipline. citeturn20view4turn13view0

| Фильтр | Что проверить | Если подтверждается |
|---|---|---|
| `killed by membership validation` | Unknown/stale sender never reaches consensus logic because current overlay/session membership is checked first | Закрыть как FP или понизить до hardening note |
| `killed by signature check` | Все dangerous branches unreachable before signature / certificate validation on current session data | Закрыть или переопределить attacker model |
| `killed by slot/window bound` | Путь ограничен строгим max slot / leader-window / frontier rule so adversary cannot scale pending work materially | Обычно hardening note, не bounty-grade |
| `killed by per-slot pending state` | Есть strong one-per-slot or one-per-key pending gate, и cross-slot amplification тоже жёстко ограничена | Понизить severity; often not exploitable enough |
| `killed by dedup/replay protection` | Correct object cannot be suppressed by invalid twin, and dedup happens only after full validation | Закрыть replay/duplicate claim |
| `Byzantine-only but unrealistic` | Атака требует custom malicious validator code or operator-local corruption with no plausible remote path | Оставить только если impact on safety/liveness доказан и attacker model challenge-compatible |
| `only hardening note` | Нет crash/split/halt/OOM/measurable liveness loss; есть лишь отсутствие local cap or suboptimal ordering | Не готово к репорту; keep as hardening idea |
| `not in scope` | Проблема вне `validator/consensus/` и не влияет напрямую на новый consensus algorithm | Не тратить validation time в текущем challengescope |
| `no reproduction path` | Есть красивая theory, but no safe local/cloud-only harness or deterministic static proof | Не репортить; сначала добрать evidence |
| `future-data leakage` | Гипотеза “подтверждается” только потому, что агент опирается на поздний fix, issue, public report or later chain artifact | Считать analysis invalid и rerun in clean environment |

## Формат файла, который стоит сохранить как `security-notes/research-memory/historical-consensus-bug-lessons.md`

Рекомендуемый exact format — компактный markdown, который одновременно пригоден и для человека, и для Codex. Он должен быть memory file, а не энциклопедией.

```md
# Historical Consensus Bug Lessons for TON

## Scope guard
- Primary challenge scope: `validator/consensus/`
- Exclude: `validator/consensus/null`
- Reports outside the directory matter only if they directly affect the new consensus algorithm
- Reference implementation: testnet branch
- No report without self-reproduction
- Local/cloud-only validation only
- Future-data leakage forbidden

## Source quality legend
- A = official advisory / official postmortem / official release notes
- B = maintainer-authored public retrospective or technical blog
- C = curated technical secondary source used only when official source withholds technical detail

## Repro quality legend
- H = public technical detail sufficient for likely local reproduction
- M = detailed root cause and chain/version data, but no ready harness
- L = summary/postmortem only

## Method lessons from agent research
- Skills from historical incidents improve hypothesis quality.
- Tool-only agents generate weaker, less actionable findings.
- Finding the bug class is easier than proving impact/reproduction.
- Future-data leakage invalidates evaluations.
- Skeptic review is mandatory.
- Validation must be local and deterministic.

## Incident lessons
### IC-001 — [short incident title]
- Project / client:
- Date:
- Bug class:
- Affected component:
- Root cause:
- Trigger / input type:
- Attacker model:
- Impact:
- Public reproduction availability:
- Patch / remediation summary:
- Why relevant to TON:
- What Codex should learn:
- Source quality:
- Sources:

### IC-002 — ...
- ...

## Reusable TON-relevant patterns
### PAT-001 — [pattern name]
- Description:
- Code smells:
- Attacker model:
- False-positive traps:
- Evidence required:
- Safe local validation idea:
- How to apply to TON:

### PAT-002 — ...
- ...

## Codex skills
### Skill: p2p-sender-contract-review
- When to invoke:
- Required inputs:
- Procedure:
- Stop conditions:
- Output format:

### Skill: malformed-message-crash-review
- When to invoke:
- Required inputs:
- Procedure:
- Stop conditions:
- Output format:

### Skill: consensus-liveness-resource-review
- When to invoke:
- Required inputs:
- Procedure:
- Stop conditions:
- Output format:

### Skill: certificate-quorum-binding-review
- When to invoke:
- Required inputs:
- Procedure:
- Stop conditions:
- Output format:

### Skill: async-timeout-race-review
- When to invoke:
- Required inputs:
- Procedure:
- Stop conditions:
- Output format:

## TON-specific false-positive filters
### FP-001 — killed by membership validation
- How to check:
- If true:

### FP-002 — killed by signature check
- How to check:
- If true:

### FP-003 — killed by slot/window bound
- How to check:
- If true:

### FP-004 — killed by per-slot pending state
- How to check:
- If true:

### FP-005 — killed by dedup/replay protection
- How to check:
- If true:

### FP-006 — Byzantine-only but unrealistic
- How to check:
- If true:

### FP-007 — only hardening note
- How to check:
- If true:

### FP-008 — not in scope
- How to check:
- If true:

### FP-009 — no reproduction path
- How to check:
- If true:

### FP-010 — future-data leakage
- How to check:
- If true:

## How Codex should use this memory
- Start from incident families, not generic bug buzzwords.
- Map current code to one or more patterns.
- Run skeptic pass against attacker model and upstream bounds.
- Do not claim vulnerability without local/cloud-only validation.
- Prefer mechanism classification first: crash / halt / split / resource exhaustion / hardening note.
```

Если это сохранять в repo, то лучше не раздувать файл сотнями кейсов. Для первого TON pass достаточно именно того compact set, который выше: несколько десятков строк incident memory, дюжина patterns, пять skills и строгие false-positive filters. Это ближе к тому, что реально подняло качество agentic auditing в a16z/Knowdit-style workflows, и лучше соответствует challenge, чем большая несфокусированная «кладбищенская» база ссылок. citeturn20view4turn19view1turn13view0