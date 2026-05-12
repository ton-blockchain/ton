# Catchain 2.0: Simplex Consensus in TON

[Simplex](https://simplex.blog/) is a leader-based consensus protocol. Validators take turns
proposing blocks and then collectively vote on them. The protocol produces a single, ever-growing
chain of finalized blocks that every honest validator agrees on.

Simplex proceeds in slots, each representing a designated opportunity to propose a block. For
every slot, a validator is selected as the leader responsible for proposing a candidate block. The
leader broadcasts its candidate, every validator checks it, and if validation succeeds, casts a
notarization vote. Once a validator sees notarization votes totaling $\ge 2/3$ of stake, it casts a
finalization vote. A quorum of finalization votes irrevocably commits the block to the output
ledger, and the protocol moves on to the next slot.

If the leader is offline or malicious, the slot eventually times out and validators cast skip votes
instead. A quorum of skip votes lets the protocol move on without producing a block for that slot.

An honest validator never both finalizes and skips the same slot, and never notarizes two different
candidates for the same slot. Because any two quorums overlap in at least one honest validator,
these per-validator constraints lift to global guarantees: at most one candidate can be finalized
per slot, and a finalized slot can never be skipped. This is the core of safety.

Multiple notarized but non-finalized chains may temporarily coexist if, because of network
conditions or leader misbehavior, a sequence of slots is both skipped and has valid candidates. Such
forks collapse as soon as the next finalization quorum is achieved, since any candidate in the later
slot must necessarily be a descendant of a slot that was not skipped. Honest validators thus commit
a candidate to the ledger once it has a finalized descendant.

The rest of this document rigorously develops the flavor of the protocol used in TON and is organized as follows:

- Section 1 formally defines the ledger model and the voting rules that honest validators must
  follow to guarantee safety.

- Section 2 proves safety: finalized chains are always consistent, regardless of network conditions
  or adversary behavior.

- Section 3 refines the rules into a concrete protocol by adding candidate generation
  rules, formalizing timeouts, and describing mechanisms required for operation in a lossy network.
  We then prove that the resulting protocol achieves liveness: with probability $1$, the output log
  grows without bound.

- Section 4 covers practical considerations that do not affect safety or liveness but improve
  throughput, and maps concepts from this document to their counterparts in the C++ implementation.

## 1. Model and Definitions

### 1.1 System Model

We consider a system of $n$ validators $\mathcal{V} = \{v_1, \dots, v_n\}$, each with a positive
integer weight $w_i$. Let $W = \sum_i w_i$. At most $f < W/3$ total weight of validators may be
*Byzantine*. Define the quorum threshold $q = \lfloor 2W/3 \rfloor + 1$.

Each honest validator holds an EUF-CMA-resistant signing key pair; each validator's public key is known to all. No assumption is made about Byzantine validators' keys beyond possession of a public key.

### 1.2 Ledger Validity

Let $\mathcal{D}$ denote the set of all possible block payloads. We assume an external predicate $\mathsf{ValidSeq} : \mathcal{D}^* \rightarrow \{\mathsf{true}, \mathsf{false}\}$, which determines whether a sequence of payloads represents a valid ledger state. The predicate satisfies the following properties:
1. *Genesis validity.* $\mathsf{ValidSeq}(\varnothing) = \mathsf{true}$.
2. *Extendability.* For every valid sequence $(d_1,\dots,d_m)$, there exists some $d$ such that $\mathsf{ValidSeq}(d_1,\dots,d_m,d)$ holds.

A sequence $(d_1,\dots,d_m)$ is called *valid* if $\mathsf{ValidSeq}(d_1,\dots,d_m) = \mathsf{true}$.

Consensus does not depend on the internal structure of payloads beyond these properties.

### 1.3 Protocol Objects

**Slots.** *Slot number* is a discrete index $s \in \mathbb{N}_0$. Slots are grouped into *leader windows* of size $L$. Window $k$ covers slots $[kL, (k+1)L)$ with an associated leader $v_{(k \bmod n)+1}$.

**Candidates.** A *candidate* is a tuple $(s, d, s_p, h_p, \sigma)$ where
- $s$ is the slot number,
- $d \in \mathcal{D}$ is the block payload,
- $(s_p, h_p)$ is a parent reference: either $(-1, \varnothing)$ or a pair with $s_p < s$,
- $\sigma$ is a valid signature by the leader of the window containing $s$ over $(s, d, s_p, h_p)$.

For a candidate $(s, d, s_p, h_p, \sigma)$, define $h = \mathsf{hash}(d, s_p, h_p)$. The pair $(s,h)$ uniquely identifies the candidate.

**Chains.** A *chain* ending at $(s_m, h_m)$ is a sequence of candidates where each candidate's
parent reference points to the previous candidate in the sequence. Formally, it is a sequence of
candidates $(s_1,d_1,s_{p1},h_{p1},\sigma_1), \dots, (s_m,d_m,s_{pm},h_{pm},\sigma_m)$, identified by
$(s_1, h_1), \dots, (s_m, h_m)$, such that:
- the first candidate has the genesis parent: $(s_{p1},h_{p1}) = (-1,\varnothing)$, and
- each subsequent candidate references the previous one: $(s_{pi},h_{pi}) = (s_{i-1},h_{i-1})$ for $i \in [2, m]$.

The *slot set* of a chain is $\{s_1, \dots, s_m\}$.

The associated sequence $(d_1,\dots,d_m)$ is called the *chain state*.

A candidate $(s,h)$ is called *valid* if there exists a chain ending at $(s, h)$ with a state that is valid.

**Votes.** At any point, a validator may cast a *vote* signed by its signing key certifying some statement $S$. Votes can be sent over the network.

**Certificates.** A *certificate* for a statement $S$ is a set of votes for $S$ from distinct validators with total
weight $\ge q$.

We say that $S$ is *reached* in the network if a certificate for $S$ can be produced by an oracle that knows all cast votes. We say that $S$ is *observed* on validator $v$ when validator $v$ receives enough votes to
construct a certificate for $S$ (possibly by receiving an already constructed certificate). Note that a statement can be reached without any single validator knowing it.

### 1.4 Statements

We implicitly set the statements $\mathsf{Notar}(-1, \varnothing)$, $\mathsf{Final}(-1, \varnothing)$ to be reached.

A particular validator $v$ may cast votes certifying the following statements:

* $\mathsf{Notar}(s,h)$. Validator $v$ has not cast a vote for $\mathsf{Notar}(s, h')$ for $h \ne h'$, and there exists a candidate $(s,d,s_p,h_p,\sigma)$ identified by $(s,h)$ such that:
    1. $\mathsf{Notar}(s_p,h_p)$ is reached,
    2. for each slot $s_p < s' < s$, the statement $\mathsf{Skip}(s')$ is reached,
    3. the candidate $(s,h)$ is valid.

* $\mathsf{Skip}(s)$. Validator $v$ has not cast a vote for any statement $\mathsf{Final}(s,h)$.

* $\mathsf{Final}(s,h)$. $\mathsf{Notar}(s,h)$ is reached, and validator $v$ has not cast a vote for $\mathsf{Skip}(s)$.

Honest validators cast votes only when they can verify that the corresponding conditions hold. They are not required to cast votes immediately when the conditions become provable.

## 2. Safety

**Lemma 2.1 (Final–Skip exclusion).** For any slot $s$ and hash $h$, the statements
$\mathsf{Final}(s,h)$ and $\mathsf{Skip}(s)$ cannot both be reached.

*Proof.* Suppose both are reached. Each certificate has total signing weight $\ge q$. Since
$2q = 2(\lfloor 2W/3 \rfloor + 1) > 4W/3 > W + W/3 > W + f$, the honest overlap is positive: some
honest validator $v$ voted for both $\mathsf{Final}(s,h)$ and $\mathsf{Skip}(s)$. But Section 1.4 requires
that a $\mathsf{Final}$ voter has not voted $\mathsf{Skip}$, and a $\mathsf{Skip}$ voter has not
voted $\mathsf{Final}$. Contradiction. $\square$

**Lemma 2.2 (Unique notarization).** For any slot $s$, at most one $h$ can have
$\mathsf{Notar}(s,h)$ reached.

*Proof.* If $\mathsf{Notar}(s,h_1)$ and $\mathsf{Notar}(s,h_2)$ are both reached with
$h_1 \ne h_2$, the same overlap argument yields an honest validator that voted for both,
contradicting the local uniqueness constraint on $\mathsf{Notar}$. $\square$

**Lemma 2.3 (Finalization implies notarization).** If $\mathsf{Final}(s,h)$ is reached, then
$\mathsf{Notar}(s,h)$ is reached.

*Proof.* The certificate for $\mathsf{Final}(s,h)$ has honest signing weight $> 0$ (since
$q > f$). Each honest signer verified that $\mathsf{Notar}(s,h)$ is reached before
voting. $\square$

**Lemma 2.4 (Unique finalization).** For any slot $s$, at most one $h$ can have
$\mathsf{Final}(s,h)$ reached.

*Proof.* Suppose $\mathsf{Final}(s,h_1)$ and $\mathsf{Final}(s,h_2)$ are both reached with
$h_1 \ne h_2$. By Lemma 2.3, $\mathsf{Notar}(s, h_1)$ and $\mathsf{Notar}(s, h_2)$ are reached. This contradicts Lemma 2.2. $\square$

**Lemma 2.5 (Chain notarization).** If $\mathsf{Notar}(s,h)$ is reached, then for every
$(s',h')$ in the chain ending at $(s,h)$, the statement $\mathsf{Notar}(s',h')$ is reached.

*Proof.* By induction on chain length. For the head, $\mathsf{Notar}(s,h)$ is reached by
assumption. For the parent $(s_p,h_p)$, the $\mathsf{Notar}(s,h)$ conditions require
$\mathsf{Notar}(s_p,h_p)$ to be reached. Apply the inductive hypothesis to
$(s_p,h_p)$. $\square$

**Theorem 2.6 (Safety).** If $\mathsf{Final}(s_a,h_a)$ and $\mathsf{Final}(s_b,h_b)$ are both
reached with $s_a \le s_b$, then the chain ending at $(s_a,h_a)$ is a prefix of the chain ending
at $(s_b,h_b)$.

*Proof.* If $s_a = s_b$, then by Lemma 2.4, $h_a = h_b$.

Otherwise, let $C_b$ be the chain ending at $(s_b,h_b)$ with slot set $S_b$. Suppose
$s_a \notin S_b$. Since $s_a < s_b$, the chain $C_b$ must skip slot $s_a$: there exists a candidate
$(s_c, d_c, s_d, h_d, \sigma_c)$ identified by $(s_c, h_c)$ in $C_b$ with $s_d < s_a < s_c$. By Lemma 2.3, $\mathsf{Notar}(s_b,h_b)$ is reached; by Lemma 2.5, so is
$\mathsf{Notar}(s_c,h_c)$. The notarization conditions for $(s_c,h_c)$ require $\mathsf{Skip}(s_a)$ to be reached. But
$\mathsf{Final}(s_a, h_a)$ is also reached, contradicting Lemma 2.1. $\square$

Let $(s^*, h^*)$ be the candidate identifier with the largest slot $s^*$ such that $v$ has observed
$\mathsf{Final}(s^*, h^*)$, or $(-1, \varnothing)$ if no finalization has been observed. The *output
log* of $v$ is the chain state of the chain ending at $(s^*, h^*)$.

**Corollary 2.7 (Consistency).** Any two honest validators' output logs are such that one is a
prefix of the other.

*Proof.* Let $(s_a^*, h_a^*)$ and $(s_b^*, h_b^*)$ be the latest finalized identifiers observed by
validators $v_a$ and $v_b$ respectively. WLOG, $s_a^* \le s_b^*$. Both $\mathsf{Final}$ statements
are reached (since they were observed). By Theorem 2.6, the chain ending at $(s_a^*, h_a^*)$ is a
prefix of the chain ending at $(s_b^*, h_b^*)$, so the output log of $v_a$ is a prefix of the
output log of $v_b$. $\square$

The permission for $\mathsf{Notar}(s,h)$ and $\mathsf{Skip}(s)$ to coexist means non-finalized
histories may fork (a slot that is both skipped and notarized can be included or excluded by later
candidates), but Theorem 2.6 guarantees that finalization collapses any such forks into a single
consistent chain.

## 3. Liveness

Safety holds unconditionally but says nothing about progress. The voting rules in Section 1.4 are
compatible with validators never casting a single vote. To prove liveness, we need to specify when
honest validators actually vote, how leaders produce candidates, and how the network delivers
messages between validators. This section provides those operational rules (Section 3.1), introduces a
probabilistic network model (Section 3.2), and then proves that, under these rules and this model,
finalization happens infinitely often with probability $1$.

### 3.1 Honest Validator Rules

Fix parameters $T_0$ (skip-timeout scale), $T_s$ (standstill period), and $\alpha > 1$
(skip-timeout growth rate).

An honest validator $v$ maintains per-slot voting state (which $\mathsf{Notar}$,
$\mathsf{Skip}$, and $\mathsf{Final}$ votes it has cast) and a local frontier $F_v$
(Rule 1). In all cases, "broadcast" means sending the message to every other validator.

**Rule 1 (Frontier tracking).** A slot $s$ is *cleared* for $v$ if $v$ has observed
$\mathsf{Notar}(s,\cdot)$, $\mathsf{Skip}(s)$, or $\mathsf{Final}(s',\cdot)$ for some
$s' \ge s$. The frontier $F_v(t)$ is the smallest slot that is not cleared for $v$.
Initially $F_v = 0$.

When $F_v$ crosses a window boundary $kL$, window $k$ becomes *active* for $v$.

The next two rules describe how validators obtain and produce candidates.

**Rule 2 (Candidate resolution).** Honest validators store candidates they have voted
$\mathsf{Notar}$ for. Validator $v$ can obtain a notarized candidate identified by
$(s,h)$ by requesting it from a uniformly random peer and retrying after an exponentially
increasing timeout. When $v$ receives such a request for a candidate it has stored, it
replies.

Given a certificate proving that $\mathsf{Notar}(s,h)$ is reached, a node can resolve the
state of the chain at $(s,h)$ (or simply, *resolve the state at $(s,h)$*) by following
parent links and recursively requesting missing candidates from other nodes.

**Rule 3 (Leader duty).** When window $k$ becomes active and $v$ is its leader, $v$
chooses any base $(s_p,h_p)$ for which it can prove all of the following:

1. $s_p < kL$,
2. $\mathsf{Notar}(s_p,h_p)$ is reached,
3. $\mathsf{Skip}(s')$ is reached for every $s_p < s' < kL$.

Validator $v$ then resolves the state at $(s_p,h_p)$ and produces a valid
candidate $(kL,d,s_p,h_p,\sigma)$.

The candidate is broadcast to all validators.

The following three rules govern how validators vote on candidates and handle leader failures.

**Rule 4 (Notarize).** Upon receiving a candidate $(s,d,s_p,h_p,\sigma)$ identified by $(s,h)$,
validator $v$ starts state resolution for $(s_p,h_p)$. It votes $\mathsf{Notar}(s,h)$ and
broadcasts the vote as soon as it can prove all of the following:

1. $v$ has not previously voted $\mathsf{Notar}(s,\cdot)$ for any candidate at slot $s$.
2. $\mathsf{Notar}(s_p,h_p)$ is reached.
3. For every slot $s'$ with $s_p < s' < s$, $\mathsf{Skip}(s')$ is reached.
4. Candidate $(s,h)$ is valid.

**Rule 5 (Finalize).** Validator $v$ votes $\mathsf{Final}(s,h)$ and broadcasts the vote
as soon as it can prove all of the following:

1. $v$ has voted $\mathsf{Notar}(s,h)$,
2. $\mathsf{Notar}(s,h)$ is reached,
3. $v$ has not voted $\mathsf{Skip}(s)$.

**Rule 6 (Skip).** For each window $k$, after the window becomes active for $v$, and for
each slot $s$ in the window, there is a finite timeout $T_\mathsf{skip}(s) \ge T_0$ after
which $v$ votes $\mathsf{Skip}(s)$ (unless it has already voted
$\mathsf{Final}(s,\cdot)$), and broadcasts the vote.

Let $k^*$ be the window containing the largest slot $s^*$ such that $v$ can prove
$\mathsf{Final}(s^*,\cdot)$ is reached at the moment window $k$ becomes active. Then $T_\mathsf{skip}(s) \ge T_0 \cdot \alpha^{k-k^*-1}$
for every slot $s$ in window $k$.

The final two rules handle certificate propagation and recovery from stalls.

**Rule 7 (Certificate formation and rebroadcast).** When $v$ has received votes for a
statement $S$ with total weight at least $q$, it forms the corresponding certificate.
Upon forming or receiving any certificate, $v$ broadcasts it.

**Rule 8 (Standstill resolution).** When $v$ has not observed any new finalized blocks
since time $t_0$, the $j$-th standstill-resolution attempt ($j \in \mathbb{N}_0$) occurs
at time $t_0 + (j+1)T_s$. Each standstill-resolution attempt is a broadcast that sends:

1. the certificate for $\mathsf{Final}(s,\cdot)$ with largest slot $s$ that $v$ has
   observed (unless $s=-1$),
2. all certificates $v$ holds for slots $> s$,
3. all votes cast by $v$ for slots $> s$.

### 3.2 Network Model

We consider the following two-phase network model:

- **Adversarial phase** ($t < T_\mathsf{GST}$). The adversary controls message delivery
  between all pairs of validators: it may delay, reorder, or drop messages arbitrarily.
- **Good phase** ($t \ge T_\mathsf{GST}$). Each message sent by an honest validator to
  another honest validator is an independent trial: it is delivered within time $\delta$
  with probability $1-r$ and lost otherwise. Here $r \in [0,1)$ is the *drop rate*.

Unlike the standard GST model, which assumes perfect delivery after stabilization, we retain
independent message loss after $T_\mathsf{GST}$. This forces the liveness proof to account
explicitly for the operational mechanisms (standstill and candidate resolution) that a real
implementation needs anyway.

**Assumption 9 (Vote and certificate delivery).** Each vote or certificate sent from one
honest validator to another after $T_\mathsf{GST}$ is an independent trial that arrives
within $\delta$ with probability at least $p_\mathrm{sdeliv} > 0$.

**Assumption 10 (Candidate broadcast delivery).** For any slot where the designated leader
is honest and broadcasts a candidate after $T_\mathsf{GST}$, independently of other slots,
some adversarially chosen set of honest validators with total weight at least $q$ receives
that candidate within $\Delta_\mathrm{bcast}$ with probability at least
$p_\mathrm{bcast} > 0$.

**Assumption 11 (Candidate resolution success).** Each candidate-resolution request sent to
an honest validator after $T_\mathsf{GST}$ is an independent trial that returns the
candidate within $\Delta_\mathrm{resolv}$ with probability at least $p_\mathrm{resolv} > 0$.

### 3.3 Almost-sure Liveness

Let $\mathcal{H}$ be the set of honest validators.

Let $t_0 \ge T_\mathsf{GST}$, and let $s_f$ be the largest slot such that
$\mathsf{Final}(s_f,\cdot)$ is reached by time $t_0$. Consider the event

$$
E_0 := \{\text{no slot } s > s_f \text{ is ever finalized after } t_0\}.

$$

**Lemma 3.1 (Eventual dissemination of honest-held data).** Condition on $E_0$. Then, with probability $1$, every vote cast by an honest validator for
a slot $> s_f$, and every certificate for a slot $> s_f$ that is observed by an honest
validator, is eventually delivered to every other honest validator.

*Proof.* Under $E_0$, no honest validator ever observes a new finalized block after time
$t_0$. By Rule 8, every honest validator therefore performs standstill broadcasts forever,
once every $T_s$ time units. Each such broadcast contains all certificates the validator
holds above its latest observed finalization, as well as all of its own votes above that
finalization.

Fix $u,v \in \mathcal{H}$ and a specific object $X$ for a slot $> s_f$, where $X$ is
either a vote cast by $u$ or a certificate observed by $u$. By Rule 8, validator $u$
retransmits $X$ infinitely many times. By Assumption 9, each retransmission reaches $v$
within $\delta$ with probability at least $p_\mathrm{sdeliv} > 0$. Hence

$$
\Pr[\text{$v$ never receives $X$}]
= \lim_{m\to\infty}(1-p_\mathrm{sdeliv})^m
= 0.

$$
Since the set of honest sender-receiver pairs is finite, this holds simultaneously for all
of them with probability $1$. $\square$

**Lemma 3.2 (Every slot is eventually cleared under an infinite standstill).** Condition on $E_0$. Then, with probability $1$, every slot $s > s_f$ is eventually cleared
for every honest validator. Consequently, $F_v(t) \to \infty$ for every $v \in \mathcal{H}$ as $t \to \infty$.

*Proof.* Proceed by induction on $s > s_f$.

Assume that every slot $s' < s$ is eventually cleared for every honest validator. Then,
for every honest validator $v$, the local frontier $F_v$ eventually reaches at least $s$.
Hence the window containing slot $s$ eventually becomes active for every honest validator.

Now consider slot $s$. There are two cases.

1. Some honest validator eventually observes $\mathsf{Notar}(s,h)$ for some $h$.

   By Lemma 3.1, every honest validator eventually observes the same notarization
   certificate. Therefore slot $s$ is eventually cleared for every honest validator by
   Rule 1.

2. No honest validator ever observes any $\mathsf{Notar}(s,\cdot)$.

   It can be shown that in this case no honest validator can ever prove that any
   $\mathsf{Notar}(s,\cdot)$ is reached. (Broadly, if no honest validator observes a
   particular certificate, no honest validator can prove the existence of such.)

   Therefore no honest validator ever votes $\mathsf{Final}(s,\cdot)$, because Rule 5
   requires proving that some $\mathsf{Notar}(s,\cdot)$ is reached. Since the window
   containing $s$ is active and Rule 6 gives a finite skip timeout, every honest
   validator eventually votes $\mathsf{Skip}(s)$.

   The total honest weight is $W-f \ge q$, so honest skip votes alone suffice to reach
   $\mathsf{Skip}(s)$. Each honest validator includes its own skip vote in every
   subsequent standstill broadcast, so by Lemma 3.1 every honest validator eventually
   receives enough honest skip votes to observe $\mathsf{Skip}(s)$. Thus slot $s$ is
   eventually cleared for every honest validator.

In both cases, slot $s$ is eventually cleared for every honest validator. This completes
the induction. $\square$

**Lemma 3.3 (Locally provable eligible bases in sufficiently late active windows).** Condition on $E_0$. Let $k_f$ be the window containing slot $s_f$. Let
$v \in \mathcal{H}$, and let $k > k_f$ be a window that is active for $v$. Then, at the
moment window $k$ becomes active for $v$, there exists at least one base for Rule 3 that
$v$ can already prove is eligible. Moreover, any chain ending at such a base has length at
most $kL$.

*Proof.* Fix such $v$ and $k$.

If $v$ has observed some $\mathsf{Notar}(b,\cdot)$ or $\mathsf{Final}(b,\cdot)$ with
$b < kL$, let $b$ be the largest such slot. Otherwise set $b=-1$, using the implicit
genesis base.

Since window $k$ is active for $v$, every slot $s < kL$ is cleared for $v$. Also, because
$k > k_f$, no slot $s \ge kL$ is ever finalized on $E_0$. Therefore, for every slot
$s$ with $b < s < kL$, clearing cannot come from any observed finalization at or above
$s$. By maximality of $b$, it also cannot come from any observed notarization or
finalization at a slot in $(b,kL)$. Hence each such slot must be cleared by an observed
skip certificate. Thus $v$ can prove $\mathsf{Skip}(s)$ is reached for every
$b < s < kL$.

If $b=-1$, the genesis base is eligible by the implicit
$\mathsf{Notar}(-1,\varnothing)$. If $b \ge 0$, then
the observed notarization or finalization certificate at slot $b$, together with the skip
certificates for all slots in $(b,kL)$, gives $v$ a proof that $(b,\cdot)$ is an eligible
base for Rule 3. This proves existence.

Finally, any chain ending at a base with slot $b < kL$ contains strictly increasing slot
numbers, so its length is at most $b+1 \le kL$. $\square$

**Lemma 3.4 (Tail bound for state resolution).** There exist constants $C_\mathrm{res} > 0$ and $\beta > 0$ such that the following holds.

Let $m \ge 1$. Suppose an honest validator needs to resolve the state of a chain whose
length is at most $m$. Then for every $T > 0$,

$$
\Pr[\text{resolution is not completed within time } T]
\le C_\mathrm{res} m^{\beta+1} T^{-\beta}.

$$

In particular, for a base in window $k$ one may take $m \le kL$, so

$$
\Pr[\text{base state is not resolved within time } T]
\le C'_\mathrm{res} k^{\beta+1} T^{-\beta}

$$
for a suitable constant $C'_\mathrm{res} > 0$ depending only on $L$ and the protocol
parameters.

*Proof.* Consider one missing candidate on the chain. Because the chain is certified
slot-by-slot, at least one honest validator stores that candidate: honest validators store
all candidates they vote $\mathsf{Notar}$ for, and every notarized or finalized slot has
positive honest overlap.

A single resolution request chooses a uniformly random peer. The probability that the
request is addressed to an honest holder is at least $1/n$. Conditioned on that event,
Assumption 11 implies a reply within $\Delta_\mathrm{resolv}$ with probability at least
$p_\mathrm{resolv}$. Hence one request attempt succeeds within
$\Delta_\mathrm{resolv}$ with probability at least $a := \frac{p_\mathrm{resolv}}{n} > 0$.

By Rule 2, retries use exponentially increasing timeouts. Therefore there exist constants
$B,B' > 0$ such that by time $T$ at least $\left\lfloor B' \ln(T/B) \right\rfloor$ request attempts have been made whenever $T \ge B$. Hence

$$
\Pr[\text{one fixed candidate is still unresolved after time } T]
\le (1-a)^{\lfloor B' \ln(T/B) \rfloor}.

$$
Setting $\beta := -B' \ln(1-a) > 0$, the right-hand side is bounded by $C_0 T^{-\beta}$ for a suitable constant $C_0 > 0$.

Now let the chain length be at most $m$. Resolve candidates sequentially from the base
upward and allocate time $T/m$ to each candidate. Let $P$ be "some candidate on the chain is not resolved within its allotted time". By the bound above and a union bound,

$$
\Pr[P]
\le m \cdot C_0 (T/m)^{-\beta}
= C_0 m^{\beta+1} T^{-\beta}.

$$
This proves the claim with $C_\mathrm{res} = C_0$. The window-$k$ specialization follows
from Lemma 3.3. $\square$

**Lemma 3.5 (Uniform positive chance of finalization in late honest windows).** There exist constants $k_0 > k_f$ and $\varepsilon > 0$ such that the following holds.

Let $k \ge k_0$ be a window whose designated leader is honest, and let $a_k$ be the time
when window $k$ becomes active for that leader. Condition on any execution history up to
time $a_k$ in which no slot larger than $s_f$ has been finalized by time $a_k$. Then the
probability that slot $kL$ is finalized before the skip timeout in that window is at least
$\varepsilon$.

*Proof.* Fix such a window $k$, and let $u$ be its honest leader. Let $\tau_k := T_0 \alpha^{k-k_f-1}$, the lower bound from Rule 6 for skip timeouts in window $k$.

Let $(b,h_b)$ be the base chosen by $u$ under Rule 3. By Rule 3, at the moment of choice,
leader $u$ already has a local proof that $(b,h_b)$ is eligible: one certificate proving
$\mathsf{Notar}(b,h_b)$ is reached, together with certificates proving
$\mathsf{Skip}(s)$ is reached for every $b < s < kL$. By Lemma 3.3, such a base exists in
every active window $k > k_f$.

We will show that, for all sufficiently large $k$, there is a uniform positive
probability that:

1. $u$ resolves the chosen base and produces the candidate quickly,
2. the candidate reaches a quorum-weight honest set $H_k$,
3. the leader's proof of eligibility reaches every validator in $H_k$ via standstill
   broadcasts,
4. every validator in $H_k$ resolves the base state quickly,
5. notarization and finalization votes are exchanged quickly enough.

We treat these steps in order.

First, the leader must resolve the chosen base state and produce its candidate. Since any
chain ending at a base with slot $b < kL$ has length at most $kL$, Lemma 3.4 gives

$$
\Pr[\text{leader does not finish this within time } \tau_k/4]
\le C_1 k^{\beta+1} \tau_k^{-\beta}

$$
for some constant $C_1 > 0$.

Second, once the candidate is produced and broadcast, Assumption 10 implies that with
probability at least $p_\mathrm{bcast}$ some honest set $H_k \subseteq \mathcal{H}$ with total weight at least $q$ receives the candidate within
$\Delta_\mathrm{bcast}$.

Third, the validators in $H_k$ must receive enough certificates to prove Rule 4(2) and 4(3)
for the leader's chosen base. Let $P_k$ be the set of certificates needed for that proof:
one certificate for the base, and one skip certificate for each slot between $b$ and $kL$.
Thus $|P_k| \le kL+1$.

As long as no new finalized block is observed, Rule 8 makes the leader rebroadcast all of
these certificates every $T_s$ time units. Over any interval of length $\tau_k/2$, there
are at least

$$
M_k := \left\lfloor \frac{\tau_k}{2T_s} \right\rfloor - 1

$$
standstill attempts once $k$ is large enough. For any fixed certificate
$C \in P_k$ and any fixed validator $w \in H_k$, the probability that none of those
$M_k$ transmissions from $u$ to $w$ arrives within $\delta$ is at most $(1-p_\mathrm{sdeliv})^{M_k}$.
Let $P'$ be "some validator in $H_k$ is still missing some certificate in $P_k$ after time $\tau_k/2$".
A union bound over all certificate-recipient pairs shows that

$$
\Pr[P']
\le n(kL+1)(1-p_\mathrm{sdeliv})^{M_k}.

$$
The right-hand side tends to $0$ exponentially fast in $\tau_k$.

Fourth, each validator in $H_k$ must resolve the same base state before voting
$\mathsf{Notar}(kL,\cdot)$. Applying Lemma 3.4 and union bounding over at most $n$
validators,

$$
\Pr[\text{some validator in } H_k \text{ fails to resolve within time } \tau_k/4]
\le C_2 k^{\beta+1} \tau_k^{-\beta}

$$
for some constant $C_2 > 0$.

Fifth, the resulting notarization and finalization votes must be exchanged quickly enough.
Choose some collector $c_k \in H_k$. Let $E_\mathrm{vote}$ be the event that:

1. every validator in $H_k$ delivers its notarization vote to $c_k$ within $\delta$;
2. $c_k$ delivers the resulting notarization certificate to every validator in $H_k$
   within $\delta$;
3. every validator in $H_k$ delivers its finalization vote to $c_k$ within $\delta$.

This involves at most $3n$ point-to-point deliveries between honest validators. By
Assumption 9,

$$
\Pr[E_\mathrm{vote}] \ge p_\mathrm{sdeliv}^{3n}.

$$

Now define the success event $E_k$ as the conjunction of the following:

1. the leader resolves the chosen base and produces the candidate within $\tau_k/4$;
2. the candidate reaches an honest set $H_k$ of total weight at least $q$ within
   $\Delta_\mathrm{bcast}$;
3. by time $\tau_k/2$, every validator in $H_k$ has received all certificates in $P_k$;
4. by time $\tau_k/2 + \tau_k/4$, every validator in $H_k$ has resolved the base state;
5. $E_\mathrm{vote}$ occurs.

On $E_k$, by time $\tau_k/2 + \tau_k/4$ every validator in $H_k$ has all information
needed to prove Rule 4(2)--(4). Moreover, because the leader is honest and produces
only one candidate per slot, no other properly signed candidate for slot $kL$ exists,
so no validator in $H_k$ can have previously voted $\mathsf{Notar}(kL,\cdot)$ for a
different candidate, satisfying Rule 4(1). Thus all validators in $H_k$ vote
$\mathsf{Notar}(kL,\cdot)$. The collector forms the notarization certificate and
rebroadcasts it. Since all of these validators have not yet skipped slot $kL$, they then
vote $\mathsf{Final}(kL,\cdot)$, and the collector receives finalization votes of total
weight at least $q$.

The total time used on $E_k$ is at most $\frac{\tau_k}{2} + \frac{\tau_k}{4} + \Delta_\mathrm{bcast} + 3\delta$.
Because $\tau_k$ grows exponentially in $k$ while $\Delta_\mathrm{bcast}$ and $\delta$
are constant, there exists $k_0 > k_f$ such that for all $k \ge k_0$, $\Delta_\mathrm{bcast} + 3\delta \le \frac{\tau_k}{4}$, and simultaneously

$$
C_1 k^{\beta+1}\tau_k^{-\beta}
+ C_2 k^{\beta+1}\tau_k^{-\beta}
+ n(kL+1)(1-p_\mathrm{sdeliv})^{M_k}
\le \frac{1}{2}.

$$

For such $k$, the chain rule gives

$$
\Pr[E_k]
\ge \frac{1}{2}\, p_\mathrm{bcast}\, p_\mathrm{sdeliv}^{3n}.

$$
Set $\varepsilon := \frac{1}{2}\, p_\mathrm{bcast}\, p_\mathrm{sdeliv}^{3n} > 0$.

Finally, on $E_k$ the finalization certificate for slot $kL$ is formed before time
$\tau_k$, hence before any honest validator's skip timeout for that slot expires.
Therefore slot $kL$ is finalized before timeout. Thus every sufficiently late honest-leader
window succeeds with conditional probability at least $\varepsilon$. $\square$

**Theorem 3.6 (A later finalization occurs almost surely).** Fix any finite execution prefix ending at time $t_0 \ge T_\mathsf{GST}$, and let $s_f$ be
the largest slot finalized by time $t_0$. Then, conditioned on that prefix, with
probability $1$ some slot $s > s_f$ is eventually finalized.

*Proof.* Let $E_0$ be the event that no slot larger than $s_f$ is ever finalized.

By Lemma 3.2, on $E_0$ every honest validator's frontier tends to infinity. Therefore
every sufficiently late window eventually becomes active for every honest validator. Since
leader windows rotate cyclically and the honest stake is positive, infinitely many of
those active windows have honest leaders.

Enumerate, on the event $E_0$, the honest-leader windows with index at least $k_0$ from
Lemma 3.5 as $K_1 < K_2 < \cdots$.
For each $i$, let $A_i$ be the event that the leader slot of window $K_i$ is finalized
before its skip timeout.

Lemma 3.5 states that for every $i$, $\Pr[A_i \mid \mathcal{F}_i] \ge \varepsilon$, where $\mathcal{F}_i$ is the full execution history up to the activation time of window
$K_i$, under the condition that no slot larger than $s_f$ has been finalized before that
activation time. Hence, by the chain rule,

$$
\Pr\!\left[\bigcap_{i=1}^m A_i^c\right] \le (1-\varepsilon)^m
\qquad\text{for every } m \ge 1.

$$

But on $E_0$, all of the windows $K_i$ exist and all of the events $A_i$ fail. Therefore

$$
\Pr[E_0]
\le \Pr\!\left[\bigcap_{i=1}^m A_i^c\right]
\le (1-\varepsilon)^m
\qquad\text{for every } m \ge 1.

$$
Letting $m \to \infty$ yields $\Pr[E_0] = 0$. Thus, conditioned on the execution prefix
up to time $t_0$, some later slot is finalized almost surely. $\square$

**Theorem 3.7 (Infinitely many finalized blocks).** With probability $1$, infinitely many slots are finalized.

*Proof.* Define a sequence of random slots $(S_j)_{j \ge 0}$ recursively by $S_0 := -1$, and for each $j \ge 0$, let $S_{j+1}$ be the smallest slot strictly larger than $S_j$
that is finalized, if such a slot exists.

Theorem 3.6 applies after any finite execution prefix. In particular, on the event that
$S_j$ exists, condition on the history up to the time when $S_j$ is finalized. Then, with
probability $1$, some later slot is finalized. Equivalently,

$$
\Pr[S_{j+1} < \infty \mid S_j < \infty] = 1
\qquad\text{for every } j \ge 0.

$$

By induction on $j$,

$$
\Pr[S_j < \infty] = 1
\qquad\text{for every } j \ge 0.

$$
Hence, with probability $1$, the sequence $(S_j)$ is defined for all $j$, which means
that infinitely many slots are finalized. $\square$

Theorem 3.7 establishes almost-sure liveness. Under the operational rules of Section 3.1 and the
probabilistic post-$T_\mathsf{GST}$ network model of Section 3.2, the protocol finalizes blocks
infinitely often with probability $1$. Combined with the safety theorem from Section 2, this
implies that honest validators' output logs form a single ever-growing chain.

## 4. Implementation Notes

### 4.1 Concept-to-Code Map

In the C++ node, the implementation lives in `validator/consensus/`. The following table maps
protocol concepts to their code counterparts:

| Protocol concept | Code |
|---|---|
| Slot | `CandidateId::slot` (`types.h`); per-slot state in `ConsensusState::slots_` (`simplex/state.h`) |
| $L$ | `slots_per_leader_window` in `NewConsensusConfig::Simplex` |
| Leader rotation | `SimplexCollatorSchedule::expected_collator_for`: $\lfloor s/L \rfloor \bmod n$ (`simplex/bus.cpp`) |
| Candidate | `struct Candidate` (`types.h`): id, parent, block data, leader signature |
| $\mathsf{Notar}$/$\mathsf{Skip}$/$\mathsf{Final}$ votes | `NotarizeVote`, `SkipVote`, `FinalizeVote` (`simplex/votes.h`) |
| Certificates | `Certificate<T>` template (`simplex/certificate.h`); aliases `NotarCert`, `SkipCert`, `FinalCert` |
| Frontier $F_v$ | `PoolImpl::now_` (`simplex/pool.cpp`): smallest slot not yet notarized or skipped |
| Rule 1 (frontier tracking) | `PoolImpl::advance_present` advances `now_`; `ConsensusState::notify_finalized` prunes old slots |
| Rule 2 (candidate resolution) | `CandidateResolverImpl` (`simplex/candidate-resolver.cpp`): random-peer fetch with exponential backoff (0.5 s initial, 1.5$\times$, 30 s cap) |
| Rule 3 (leader duty) | `BlockProducerImpl::generate_candidates` (`block-producer.cpp`): produces $L$ candidates per window |
| Rule 4 (notarize) | `ConsensusImpl::try_notarize` (`simplex/consensus.cpp`): async pipeline of parent wait $\to$ state resolve $\to$ validate $\to$ vote |
| Rule 5 (finalize) | `ConsensusImpl::try_vote_final` (`simplex/consensus.cpp`) |
| Rule 6 (skip) | `ConsensusImpl::alarm` (`simplex/consensus.cpp`): fires after `first_block_timeout_s_` per window |
| Rule 7 (cert formation/rebroadcast) | `PoolImpl::handle_vote` (`simplex/pool.cpp`) forms certs at quorum; `PoolImpl::handle_our_certificate` rebroadcasts obtained certificates |
| Rule 8 (standstill) | `PoolImpl::alarm` (`simplex/pool.cpp`): fires every 10 s, broadcasts all held certs and own votes |

Communication uses a private validator overlay (`simplex/private-overlay.cpp`). Votes and
certificates are sent individually to each peer. Candidates are broadcast using two-step FEC
erasure coding (`overlay/broadcast-twostep.cpp`), where the data is split into $k \approx 2n/3$
symbols and each peer receives one symbol.

### 4.2 Deviations from the Rules

Rule 3 only requires the leader to produce one candidate per window. The implementation produces
$L$: `generate_candidates` loops over all slots in the window, producing one candidate per slot
with each referencing the previous as its parent. Later candidates in the window begin generation
immediately without waiting for earlier ones to be notarized, since the leader already knows the
only possible parent — it just produced it.

The implementation sometimes suppresses votes that the Section 3.1 rules would require. None of these
affect liveness, since in all such situations the suppressed vote would not lead to formation of
a new certificate for a non-finalized slot.

Rule 6 requires $T_\mathsf{skip}(s) \ge T_0 \cdot \alpha^{k - k^* - 1}$, where $k^*$ is
determined by the last finalization that was reached. The implementation currently resets the
timeout upon seeing a leader window without skip votes — this is a bug and will be fixed. We
additionally cap $T_\mathsf{skip}(s)$ at 100 s, in the expectation that any reasonable consensus
synchronization will not require longer skip timeouts. Regardless, a mechanism to manually
increase `first_block_max_timeout_s` is planned.

### 4.3 Network Model Discussion

Assumptions 10 and 11 require bounded delivery times $\Delta_\mathrm{bcast}$ and
$\Delta_\mathrm{resolv}$. This is justified because the implementation caps candidate
payload size at a constant $C_\mathsf{max}$, so delivery time is
$\delta + \Theta(C_\mathsf{max})$ for both broadcast and resolution.

The egress induced by the protocol is currently unbounded. In particular, a sufficiently long
period of asynchrony (or a bug) can induce a state whose synchronization saturates link capacity,
causing unsynchronized state to only grow further — a death spiral. We plan to address this with
better timeout tuning and optimizations to the standstill resolution mechanism.

The two-step FEC encoding uses $k \approx 2n/3$ data symbols distributed one per peer.
Reconstruction requires any $k$ of the $n$ symbols, tolerating at most $n - k \approx n/3$
losses. This is a count threshold, not a weight threshold. We consider this acceptable since we
do not expect the number of Byzantine validators to significantly exceed their share of total
weight.

The network model requires drop probabilities to be independent across packets. In practice,
QUIC is more likely to drop packets after a burst of high momentary egress, even when it could
have spread the load over time. We plan to address this by more precise tracking of the moment
each message becomes irrelevant for the protocol, allowing earlier cancellation and reducing
unnecessary egress.

### 4.4 Empty Candidates

The leader sometimes produces an empty candidate — one that references an existing block by
its `BlockIdExt` rather than carrying new block data. Empty candidates participate in consensus
normally (they are notarized, finalized, and form chains) but do not advance any user-visible
blockchain state.

Empty candidates are generated in two situations, both driven by constraints external to
consensus:

1. A shardchain leader generates empty candidates when the masterchain's last finalized seqno
   falls more than 8 blocks behind the shard's current tip. This is a legacy validation
   requirement that prevents the shardchain from building an arbitrarily long tail of blocks
   that the masterchain has not yet acknowledged.

2. A masterchain leader generates empty candidates when the next block to produce would be more
   than one seqno ahead of the last consensus-finalized block. This ensures that every
   masterchain block eventually receives finalization signatures, which all non-validator nodes
   depend on.
