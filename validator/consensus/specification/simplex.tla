---- MODULE simplex ----
\* Simplex consensus (TON Catchain 2.0) in TLA+.
\*
\* A few abstractions worth knowing about up front:
\*   - No real-time clock. Timeouts are non-deterministic actions guarded
\*     by window activation; fairness gives us the "finite timeout" of
\*     Rule 6.
\*   - Post-GST probabilistic delivery is replaced by non-determinism
\*     plus fairness on delivery.
\*   - ValidSeq is left as an abstract predicate (default TRUE). The
\*     protocol's safety doesn't care about payload contents.
\*   - Candidates live per-validator in knownCandidates[v]: a vote on a
\*     candidate requires having received it first.
\*   - Leaders pick bases from their own observed state, not the global
\*     oracle.  This matches what a real node can prove locally.

EXTENDS Integers, Sequences, FiniteSets, TLC, Naturals

\* ---- system parameters -----------------------------------------------------

CONSTANT NumValidators            \* n
CONSTANT ByzantineSet             \* subset of 1..NumValidators; weight < W/3
CONSTANT Weight(_)                \* w_i; 1-arg operator so TLC can substitute
CONSTANT LeaderWindowSize         \* L

\* ---- model-checking bounds -------------------------------------------------

CONSTANT MaxSlot                  \* keeps the state space finite
CONSTANT MaxMessages              \* in-flight cap

CONSTANT EnableMessageDrop        \* adversarial phase on/off
CONSTANT EnableByzantineEquivocation
CONSTANT EnableByzantineWithholding

\* ValidSeq abstraction. The spec only relies on (a) genesis validity and
\* (b) extendability, both of which we get for free by defaulting to TRUE.
\* If you want payload checks, override in the config.
CONSTANT ValidSeq(_)

\* ---- derived sets ----------------------------------------------------------

Validators == 1..NumValidators
HonestValidators == Validators \ ByzantineSet

\* W = sum of weights.  Encoded as a folded recursion since TLA+ doesn't
\* give us a primitive sum over a set.
TotalWeight == LET Sum[S \in SUBSET Validators] ==
                   IF S = {} THEN 0
                   ELSE LET v == CHOOSE x \in S : TRUE
                        IN Weight(v) + Sum[S \ {v}]
               IN Sum[Validators]

QuorumThreshold == (2 * TotalWeight) \div 3 + 1     \* q

ByzantineWeight == LET Sum[S \in SUBSET Validators] ==
                       IF S = {} THEN 0
                       ELSE LET v == CHOOSE x \in S : TRUE
                            IN Weight(v) + Sum[S \ {v}]
                   IN Sum[ByzantineSet]                          \* f

Slots == 0..MaxSlot
HashValues == Validators \cup {0}

\* Genesis: Notar(-1, 0) and Final(-1, 0) are reached implicitly.
GenesisSlot == -1
GenesisHash == 0

\* Leader rotation.  Window k spans slots [kL, (k+1)L), led by validator
\* (k mod n) + 1.
LeaderOf(slot) == ((slot \div LeaderWindowSize) % NumValidators) + 1
WindowOf(slot) == slot \div LeaderWindowSize
FirstSlotInWindow(k) == k * LeaderWindowSize
LastSlotInWindow(k)  == (k + 1) * LeaderWindowSize - 1

IsHonest(v) == v \notin ByzantineSet

WeightOf(S) == LET Sum[T \in SUBSET Validators] ==
                   IF T = {} THEN 0
                   ELSE LET v == CHOOSE x \in T : TRUE
                        IN Weight(v) + Sum[T \ {v}]
               IN Sum[S]

IsQuorum(S) == WeightOf(S) >= QuorumThreshold

\* ---- state -----------------------------------------------------------------

VARIABLES
    \* per-validator votes
    notarVotes,         \* set of <<slot, hash>> per validator
    skipVotes,          \* set of slot per validator
    finalVotes,         \* set of <<slot, hash>> per validator

    \* per-validator "observed" certificates (what each node can prove locally)
    observedNotar,
    observedSkip,
    observedFinal,

    \* global "reached" oracle: what would be visible to an oracle that sees
    \* every cast vote.  Distinct from observed: a statement can be reached
    \* without any single node knowing it yet.
    reachedNotar,
    reachedSkip,
    reachedFinal,

    frontier,           \* smallest slot not yet cleared for v
    knownCandidates,    \* per-validator candidate store; record has
                        \* [slot, hash, parentSlot, parentHash, leader]
    messages,           \* in-flight messages
    droppedMessages     \* dropped messages; kept around for diagnostics

vars == <<notarVotes, skipVotes, finalVotes,
          observedNotar, observedSkip, observedFinal,
          reachedNotar, reachedSkip, reachedFinal,
          frontier, knownCandidates,
          messages, droppedMessages>>

\* ---- message constructors --------------------------------------------------

NotarVoteMsg(src, dst, slot, hash) ==
    [type |-> "notarVote", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

SkipVoteMsg(src, dst, slot) ==
    [type |-> "skipVote", src |-> src, dst |-> dst, slot |-> slot]

FinalVoteMsg(src, dst, slot, hash) ==
    [type |-> "finalVote", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

NotarCertMsg(src, dst, slot, hash) ==
    [type |-> "notarCert", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

SkipCertMsg(src, dst, slot) ==
    [type |-> "skipCert", src |-> src, dst |-> dst, slot |-> slot]

FinalCertMsg(src, dst, slot, hash) ==
    [type |-> "finalCert", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

CandidateMsg(src, dst, slot, hash, parentSlot, parentHash) ==
    [type |-> "candidate", src |-> src, dst |-> dst,
     slot |-> slot, hash |-> hash,
     parentSlot |-> parentSlot, parentHash |-> parentHash]

CandidateRequestMsg(src, dst, slot, hash) ==
    [type |-> "candidateReq", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

CandidateReplyMsg(src, dst, slot, hash, parentSlot, parentHash) ==
    [type |-> "candidateReply", src |-> src, dst |-> dst,
     slot |-> slot, hash |-> hash,
     parentSlot |-> parentSlot, parentHash |-> parentHash]

\* Standstill bundles the cert/vote payload in a single message; in the
\* spec we deliver each enclosed piece as its own message for simplicity.
StandstillMsg(src, dst) ==
    [type |-> "standstill", src |-> src, dst |-> dst]

\* ---- initial state ---------------------------------------------------------

Init ==
    /\ notarVotes = [v \in Validators |-> {}]
    /\ skipVotes  = [v \in Validators |-> {}]
    /\ finalVotes = [v \in Validators |-> {}]
    /\ messages   = {}
    /\ knownCandidates = [v \in Validators |-> {}]
    /\ observedNotar = [v \in Validators |-> {}]
    /\ observedSkip  = [v \in Validators |-> {}]
    /\ observedFinal = [v \in Validators |-> {}]
    /\ reachedNotar = {}
    /\ reachedSkip  = {}
    /\ reachedFinal = {}
    /\ frontier = [v \in Validators |-> 0]
    /\ droppedMessages = {}

\* ---- certificate helpers ---------------------------------------------------
\* A certificate is just the set of distinct voters whose combined weight
\* meets the quorum threshold.

NotarVotersFor(slot, hash)  == {v \in Validators : <<slot, hash>> \in notarVotes[v]}
SkipVotersFor(slot)          == {v \in Validators : slot \in skipVotes[v]}
FinalVotersFor(slot, hash)  == {v \in Validators : <<slot, hash>> \in finalVotes[v]}

NotarReached(slot, hash) == WeightOf(NotarVotersFor(slot, hash)) >= QuorumThreshold
SkipReached(slot)        == WeightOf(SkipVotersFor(slot))         >= QuorumThreshold
FinalReached(slot, hash) == WeightOf(FinalVotersFor(slot, hash)) >= QuorumThreshold

\* ---- candidate-knowledge helpers -------------------------------------------

ValidatorKnowsCandidate(v, slot, hash) ==
    \E c \in knownCandidates[v] : c.slot = slot /\ c.hash = hash

GetKnownCandidate(v, slot, hash) ==
    CHOOSE c \in knownCandidates[v] : c.slot = slot /\ c.hash = hash

\* Convenience predicates used in invariants.
AnyCandidateExists(slot, hash) ==
    \E v \in Validators : ValidatorKnowsCandidate(v, slot, hash)

AnyKnownCandidate(slot, hash) ==
    LET v == CHOOSE v \in Validators : ValidatorKnowsCandidate(v, slot, hash)
    IN GetKnownCandidate(v, slot, hash)

\* ---- chain walking ---------------------------------------------------------
\* Walks the parent chain of (s, h) and returns the hash recorded at the
\* given target slot, or -1 if (target, _) is not on the chain.  Used by
\* the chain-prefix safety invariant.

RECURSIVE WalkChainHashAt(_, _, _)
WalkChainHashAt(target, s, h) ==
    IF s = target THEN h
    ELSE IF s <= target THEN -1
    ELSE IF ~AnyCandidateExists(s, h) THEN -1
    ELSE LET c == AnyKnownCandidate(s, h)
         IN IF c.parentSlot < target THEN -1
            ELSE WalkChainHashAt(target, c.parentSlot, c.parentHash)

\* Rule-4 validity predicate, expanded recursively over the parent chain.
\* VoteNotarize uses the inline form, so this is here mainly for clarity
\* (and as a sanity check when extending the spec).
RECURSIVE ChainValidForValidator(_, _, _)
ChainValidForValidator(v, slot, hash) ==
    IF slot = GenesisSlot THEN TRUE
    ELSE
        /\ ValidatorKnowsCandidate(v, slot, hash)
        /\ LET c == GetKnownCandidate(v, slot, hash)
           IN /\ (c.parentSlot = GenesisSlot \/
                  <<c.parentSlot, c.parentHash>> \in observedNotar[v])
              /\ \A s \in (c.parentSlot+1)..(slot-1) : s \in observedSkip[v]
              /\ (c.parentSlot = GenesisSlot \/
                  ChainValidForValidator(v, c.parentSlot, c.parentHash))

\* Window k is active for v once v's frontier crosses kL.
WindowActiveFor(v, k) == frontier[v] >= FirstSlotInWindow(k)

\* ---- Rule 1: frontier tracking ---------------------------------------------
\* A slot is cleared once v has seen any of (Notar(s,_), Skip(s), Final(s',_)
\* with s' >= s).  The frontier is the smallest still-uncleared slot.

SlotClearedFor(v, s) ==
    \/ \E h \in HashValues : <<s, h>> \in observedNotar[v]
    \/ s \in observedSkip[v]
    \/ \E s2 \in Slots : \E h \in HashValues :
        s2 >= s /\ <<s2, h>> \in observedFinal[v]

UpdateFrontier(v) ==
    /\ IsHonest(v)
    /\ LET f == frontier[v]
       IN /\ SlotClearedFor(v, f)
          /\ f < MaxSlot
          /\ frontier' = [frontier EXCEPT ![v] = f + 1]
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, messages,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   droppedMessages>>

\* ---- Rule 2: candidate resolution ------------------------------------------
\* A node that knows a notarization cert but doesn't yet have the candidate
\* asks a peer for it.  Retries with exponential backoff become arbitrary
\* re-tries here; the model checker explores all interleavings.

RequestCandidate(v, dst, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    /\ ~ValidatorKnowsCandidate(v, slot, hash)
    /\ <<slot, hash>> \in observedNotar[v]    \* must have a reason to ask
    /\ messages' = messages \cup {CandidateRequestMsg(v, dst, slot, hash)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* Reply with a stored candidate.  No reply if we don't have it.
ReplyCandidate(v, msg) ==
    /\ IsHonest(v)
    /\ msg \in messages
    /\ msg.type = "candidateReq"
    /\ msg.dst = v
    /\ ValidatorKnowsCandidate(v, msg.slot, msg.hash)
    /\ LET c == GetKnownCandidate(v, msg.slot, msg.hash)
       IN messages' = (messages \ {msg}) \cup
            {CandidateReplyMsg(v, msg.src, c.slot, c.hash, c.parentSlot, c.parentHash)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* ---- Rule 3: leader duty ---------------------------------------------------
\* The leader's three conditions are checked against its own observed state
\* (observedNotar / observedSkip).  Real nodes can't see the global oracle,
\* and neither does this action.  An honest leader produces exactly one
\* candidate per slot (the inner guard rules out a second one).
\* The candidate's hash is taken to be the leader's id; that gives us a
\* unique candidate per honest-leader/slot pair without needing a real
\* hash function.

ProposeCandidate(leader, slot) ==
    /\ slot \in Slots
    /\ slot <= MaxSlot
    /\ LeaderOf(slot) = leader
    /\ IsHonest(leader)
    /\ WindowActiveFor(leader, WindowOf(slot))
    /\ \E parentSlot \in {GenesisSlot} \cup (0..(slot-1)) :
        \E parentHash \in HashValues :
            /\ parentSlot < FirstSlotInWindow(WindowOf(slot))         \* (1)
            /\ (parentSlot = GenesisSlot \/
                <<parentSlot, parentHash>> \in observedNotar[leader]) \* (2)
            /\ \A s \in (parentSlot+1)..(slot-1) :
                   s \in observedSkip[leader]                         \* (3)
            /\ ~(\E c \in knownCandidates[leader] :
                    c.slot = slot /\ c.leader = leader)
            /\ LET hash == leader
                   cand == [slot |-> slot, hash |-> hash,
                            parentSlot |-> parentSlot,
                            parentHash |-> parentHash,
                            leader |-> leader]
               IN
                /\ knownCandidates' = [knownCandidates EXCEPT
                    ![leader] = @ \cup {cand}]
                /\ messages' = messages \cup
                    {CandidateMsg(leader, dst, slot, hash, parentSlot, parentHash) :
                     dst \in Validators \ {leader}}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* ---- Rule 4: notarize ------------------------------------------------------
\* Vote Notar(s,h) once we've seen the candidate, haven't voted for a
\* different hash at this slot already, and can prove parent + intermediate
\* skips from our own observed state.  ValidSeq is the abstract payload
\* predicate; default TRUE here.

VoteNotarize(v, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    /\ ~(\E h2 \in HashValues : h2 /= hash /\ <<slot, h2>> \in notarVotes[v])
    /\ <<slot, hash>> \notin notarVotes[v]
    /\ ValidatorKnowsCandidate(v, slot, hash)
    /\ LET c == GetKnownCandidate(v, slot, hash)
       IN
        /\ (c.parentSlot = GenesisSlot \/
            <<c.parentSlot, c.parentHash>> \in observedNotar[v])
        /\ \A s \in (c.parentSlot+1)..(slot-1) : s \in observedSkip[v]
    /\ notarVotes' = [notarVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
    /\ messages' = messages \cup
        {NotarVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
    /\ LET newVoters == NotarVotersFor(slot, hash) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedNotar' = reachedNotar \cup {<<slot, hash>>}
          ELSE reachedNotar' = reachedNotar
    /\ UNCHANGED <<skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedSkip, reachedFinal, frontier, droppedMessages>>

\* ---- Rule 5: finalize ------------------------------------------------------
\* Vote Final(s,h) if we already voted Notar(s,h), have observed that
\* Notar(s,h) is reached, and haven't skipped slot s.

VoteFinalize(v, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    /\ <<slot, hash>> \in notarVotes[v]
    /\ <<slot, hash>> \in observedNotar[v]
    /\ slot \notin skipVotes[v]
    /\ <<slot, hash>> \notin finalVotes[v]
    /\ finalVotes' = [finalVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
    /\ messages' = messages \cup
        {FinalVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
    /\ LET newVoters == FinalVotersFor(slot, hash) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedFinal' = reachedFinal \cup {<<slot, hash>>}
          ELSE reachedFinal' = reachedFinal
    /\ UNCHANGED <<notarVotes, skipVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, frontier, droppedMessages>>

\* ---- Rule 6: skip ----------------------------------------------------------
\* Skip the slot if its window is active and we haven't already finalized
\* it.  The exponential growth of T_skip is irrelevant for safety; what
\* matters here is the mutual exclusion with Final, which is the local
\* constraint that lifts to the global Final/Skip exclusion lemma.

VoteSkip(v, slot) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    \* Section 1.4: "v has not cast a vote for any Final(s,h)"
    /\ ~(\E h \in HashValues : <<slot, h>> \in finalVotes[v])
    \* Not already voted skip for this slot
    /\ slot \notin skipVotes[v]
    \* Rule 6: Window containing slot must be active for v
    /\ WindowActiveFor(v, WindowOf(slot))
    /\ skipVotes' = [skipVotes EXCEPT ![v] = @ \cup {slot}]
    /\ messages' = messages \cup
        {SkipVoteMsg(v, dst, slot) : dst \in Validators \ {v}}
    /\ LET newVoters == SkipVotersFor(slot) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedSkip' = reachedSkip \cup {slot}
          ELSE reachedSkip' = reachedSkip
    /\ UNCHANGED <<notarVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedFinal, frontier, droppedMessages>>

\* ---- Rule 7: certificate rebroadcast ---------------------------------------
\* Once we observe a certificate, push it to a peer that hasn't seen it
\* yet.  The "form a certificate at quorum" step happens inside the vote
\* delivery handler below; this action handles the "rebroadcast" half.

RebroadcastNotarCert(v, slot, hash) ==
    /\ <<slot, hash>> \in observedNotar[v]
    /\ \E dst \in Validators \ {v} :
        /\ <<slot, hash>> \notin observedNotar[dst]
        /\ messages' = messages \cup {NotarCertMsg(v, dst, slot, hash)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

RebroadcastSkipCert(v, slot) ==
    /\ slot \in observedSkip[v]
    /\ \E dst \in Validators \ {v} :
        /\ slot \notin observedSkip[dst]
        /\ messages' = messages \cup {SkipCertMsg(v, dst, slot)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

RebroadcastFinalCert(v, slot, hash) ==
    /\ <<slot, hash>> \in observedFinal[v]
    /\ \E dst \in Validators \ {v} :
        /\ <<slot, hash>> \notin observedFinal[dst]
        /\ messages' = messages \cup {FinalCertMsg(v, dst, slot, hash)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* ---- Rule 8: standstill resolution -----------------------------------------
\* When nothing has finalized in a while, dump everything we know above
\* the last finalized slot to a peer.  We split the rule's bundled payload
\* into individual cert/vote sends; a fair scheduler covers every piece.

LatestObservedFinalSlot(v) ==
    IF observedFinal[v] = {} THEN GenesisSlot
    ELSE LET maxSlot == CHOOSE s \in {p[1] : p \in observedFinal[v]} :
              \A s2 \in {p[1] : p \in observedFinal[v]} : s >= s2
         IN maxSlot

StandstillBroadcast(v) ==
    /\ IsHonest(v)
    /\ LET sF == LatestObservedFinalSlot(v)
       IN \E dst \in Validators \ {v} :
           \/ \* latest final cert (>= sF: covers the sF cert itself)
              /\ \E sh \in observedFinal[v] : sh[1] >= sF
              /\ \E sh \in observedFinal[v] :
                    messages' = messages \cup
                        {FinalCertMsg(v, dst, sh[1], sh[2])}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* a notar cert above sF
              /\ \E sh \in observedNotar[v] : sh[1] > sF
              /\ \E sh \in observedNotar[v] :
                    /\ sh[1] > sF
                    /\ messages' = messages \cup
                        {NotarCertMsg(v, dst, sh[1], sh[2])}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* a skip cert above sF
              /\ \E s \in observedSkip[v] : s > sF
              /\ \E s \in observedSkip[v] :
                    /\ s > sF
                    /\ messages' = messages \cup
                        {SkipCertMsg(v, dst, s)}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* our own notar votes above sF
              /\ \E sh \in notarVotes[v] : sh[1] > sF
              /\ \E sh \in notarVotes[v] :
                    /\ sh[1] > sF
                    /\ messages' = messages \cup
                        {NotarVoteMsg(v, dst, sh[1], sh[2])}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* our own skip votes above sF
              /\ \E s \in skipVotes[v] : s > sF
              /\ \E s \in skipVotes[v] :
                    /\ s > sF
                    /\ messages' = messages \cup
                        {SkipVoteMsg(v, dst, s)}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* our own final votes above sF
              /\ \E sh \in finalVotes[v] : sh[1] > sF
              /\ \E sh \in finalVotes[v] :
                    /\ sh[1] > sF
                    /\ messages' = messages \cup
                        {FinalVoteMsg(v, dst, sh[1], sh[2])}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* ---- Byzantine behaviour ---------------------------------------------------
\* Byzantine nodes are free to ignore every honest constraint: equivocating
\* proposals, conflicting votes, anything goes.  Witholding is modelled by
\* simply not taking these actions.

ByzantinePropose(leader, slot) ==
    /\ EnableByzantineEquivocation
    /\ slot \in Slots
    /\ slot <= MaxSlot
    /\ LeaderOf(slot) = leader
    /\ ~IsHonest(leader)
    /\ \E parentSlot \in {GenesisSlot} \cup (0..(slot-1)) :
        \E parentHash \in HashValues :
            \E hash \in HashValues :
                /\ LET cand == [slot |-> slot, hash |-> hash,
                               parentSlot |-> parentSlot,
                               parentHash |-> parentHash,
                               leader |-> leader]
                   IN
                    /\ knownCandidates' = [knownCandidates EXCEPT
                        ![leader] = @ \cup {cand}]
                    /\ messages' = messages \cup
                        {CandidateMsg(leader, dst, slot, hash, parentSlot, parentHash) :
                         dst \in Validators \ {leader}}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

ByzantineVote(v) ==
    /\ ~IsHonest(v)
    /\ EnableByzantineEquivocation \/ EnableByzantineWithholding
    /\ \E slot \in Slots :
        \/ \E hash \in HashValues :
            /\ notarVotes' = [notarVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
            /\ messages' = messages \cup
                {NotarVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
            /\ LET nv == NotarVotersFor(slot, hash) \cup {v}
               IN IF WeightOf(nv) >= QuorumThreshold
                  THEN reachedNotar' = reachedNotar \cup {<<slot, hash>>}
                  ELSE reachedNotar' = reachedNotar
            /\ UNCHANGED <<skipVotes, finalVotes, reachedSkip, reachedFinal>>
        \/ /\ skipVotes' = [skipVotes EXCEPT ![v] = @ \cup {slot}]
           /\ messages' = messages \cup
               {SkipVoteMsg(v, dst, slot) : dst \in Validators \ {v}}
           /\ LET sv == SkipVotersFor(slot) \cup {v}
              IN IF WeightOf(sv) >= QuorumThreshold
                 THEN reachedSkip' = reachedSkip \cup {slot}
                 ELSE reachedSkip' = reachedSkip
           /\ UNCHANGED <<notarVotes, finalVotes, reachedNotar, reachedFinal>>
        \/ \E hash \in HashValues :
            /\ finalVotes' = [finalVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
            /\ messages' = messages \cup
                {FinalVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
            /\ LET fv == FinalVotersFor(slot, hash) \cup {v}
               IN IF WeightOf(fv) >= QuorumThreshold
                  THEN reachedFinal' = reachedFinal \cup {<<slot, hash>>}
                  ELSE reachedFinal' = reachedFinal
            /\ UNCHANGED <<notarVotes, skipVotes, reachedNotar, reachedSkip>>
    /\ UNCHANGED <<knownCandidates, observedNotar, observedSkip, observedFinal,
                   frontier, droppedMessages>>

\* ---- network ---------------------------------------------------------------
\* Delivery is non-deterministic; the dropping action is gated by
\* EnableMessageDrop to model the adversarial phase.  Fairness on
\* DeliverMessage stands in for the post-GST "delivered with prob > 0".

DeliverMessage(msg) ==
    /\ msg \in messages
    /\ messages' = messages \ {msg}
    /\ LET dst == msg.dst
       IN
        CASE msg.type = "notarVote" ->
            /\ LET slot == msg.slot
                   hash == msg.hash
                   nv   == NotarVotersFor(slot, hash)
               IN IF WeightOf(nv) >= QuorumThreshold
                  THEN /\ observedNotar' = [observedNotar EXCEPT
                            ![dst] = @ \cup {<<slot, hash>>}]
                       /\ reachedNotar' = reachedNotar \cup {<<slot, hash>>}
                  ELSE /\ observedNotar' = observedNotar
                       /\ reachedNotar' = reachedNotar
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedSkip, observedFinal,
                           reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "skipVote" ->
            /\ LET slot == msg.slot
                   sv   == SkipVotersFor(slot)
               IN IF WeightOf(sv) >= QuorumThreshold
                  THEN /\ observedSkip' = [observedSkip EXCEPT
                            ![dst] = @ \cup {slot}]
                       /\ reachedSkip' = reachedSkip \cup {slot}
                  ELSE /\ observedSkip' = observedSkip
                       /\ reachedSkip' = reachedSkip
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedFinal,
                           reachedNotar, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "finalVote" ->
            /\ LET slot == msg.slot
                   hash == msg.hash
                   fv   == FinalVotersFor(slot, hash)
               IN IF WeightOf(fv) >= QuorumThreshold
                  THEN /\ observedFinal' = [observedFinal EXCEPT
                            ![dst] = @ \cup {<<slot, hash>>}]
                       /\ reachedFinal' = reachedFinal \cup {<<slot, hash>>}
                  ELSE /\ observedFinal' = observedFinal
                       /\ reachedFinal' = reachedFinal
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip,
                           reachedNotar, reachedSkip,
                           frontier, droppedMessages>>

        [] msg.type = "notarCert" ->
            /\ observedNotar' = [observedNotar EXCEPT
                ![dst] = @ \cup {<<msg.slot, msg.hash>>}]
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "skipCert" ->
            /\ observedSkip' = [observedSkip EXCEPT
                ![dst] = @ \cup {msg.slot}]
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "finalCert" ->
            /\ observedFinal' = [observedFinal EXCEPT
                ![dst] = @ \cup {<<msg.slot, msg.hash>>}]
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "candidate" ->
            /\ LET cand == [slot |-> msg.slot, hash |-> msg.hash,
                            parentSlot |-> msg.parentSlot,
                            parentHash |-> msg.parentHash,
                            leader |-> msg.src]
               IN knownCandidates' = [knownCandidates EXCEPT
                    ![dst] = @ \cup {cand}]
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "candidateReply" ->
            /\ LET cand == [slot |-> msg.slot, hash |-> msg.hash,
                            parentSlot |-> msg.parentSlot,
                            parentHash |-> msg.parentHash,
                            leader |-> msg.src]
               IN knownCandidates' = [knownCandidates EXCEPT
                    ![dst] = @ \cup {cand}]
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "candidateReq" ->
            \* Routed to ReplyCandidate; nothing to do here besides consume.
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "standstill" ->
            \* Standstill payload is delivered as individual cert/vote msgs.
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

DropMessage(msg) ==
    /\ EnableMessageDrop
    /\ msg \in messages
    /\ messages' = messages \ {msg}
    /\ droppedMessages' = droppedMessages \cup {msg}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal, frontier>>

\* ---- next-state relation ---------------------------------------------------

Next ==
    \/ \E v \in HonestValidators : \E s \in Slots :
        /\ LeaderOf(s) = v
        /\ ProposeCandidate(v, s)
    \/ \E v \in ByzantineSet : \E s \in Slots :
        ByzantinePropose(v, s)
    \/ \E v \in HonestValidators : \E s \in Slots : \E h \in HashValues :
        VoteNotarize(v, s, h)
    \/ \E v \in HonestValidators : \E s \in Slots : \E h \in HashValues :
        VoteFinalize(v, s, h)
    \/ \E v \in HonestValidators : \E s \in Slots :
        VoteSkip(v, s)
    \/ \E v \in ByzantineSet :
        ByzantineVote(v)
    \/ \E msg \in messages :
        DeliverMessage(msg)
    \/ \E msg \in messages :
        DropMessage(msg)
    \/ \E v \in HonestValidators :
        UpdateFrontier(v)
    \/ \E v \in HonestValidators : \E dst \in Validators :
        \E s \in Slots : \E h \in HashValues :
            RequestCandidate(v, dst, s, h)
    \/ \E v \in HonestValidators : \E msg \in messages :
        ReplyCandidate(v, msg)
    \/ \E v \in Validators : \E s \in Slots : \E h \in HashValues :
        RebroadcastNotarCert(v, s, h)
    \/ \E v \in Validators : \E s \in Slots :
        RebroadcastSkipCert(v, s)
    \/ \E v \in Validators : \E s \in Slots : \E h \in HashValues :
        RebroadcastFinalCert(v, s, h)
    \/ \E v \in HonestValidators :
        StandstillBroadcast(v)

\* ---- fairness --------------------------------------------------------------
\* Safety doesn't need any of this; it's here for liveness checks.
\* Weak fairness on honest actions: if a vote is continuously enabled, it
\* gets taken.  Strong fairness on delivery stands in for the
\* "delivered with positive probability" guarantee after GST.

Fairness ==
    /\ \A v \in HonestValidators : \A s \in Slots : \A h \in HashValues :
        WF_vars(VoteNotarize(v, s, h))
    /\ \A v \in HonestValidators : \A s \in Slots : \A h \in HashValues :
        WF_vars(VoteFinalize(v, s, h))
    /\ \A v \in HonestValidators : \A s \in Slots :
        WF_vars(VoteSkip(v, s))
    /\ \A v \in HonestValidators :
        WF_vars(UpdateFrontier(v))
    /\ \A v \in HonestValidators : \A s \in Slots :
        WF_vars(ProposeCandidate(v, s))
    /\ SF_vars(\E msg \in messages : DeliverMessage(msg))
    /\ \A v \in Validators : \A s \in Slots : \A h \in HashValues :
        WF_vars(RebroadcastNotarCert(v, s, h))
    /\ \A v \in Validators : \A s \in Slots :
        WF_vars(RebroadcastSkipCert(v, s))
    /\ \A v \in Validators : \A s \in Slots : \A h \in HashValues :
        WF_vars(RebroadcastFinalCert(v, s, h))

Spec       == Init /\ [][Next]_vars /\ Fairness
SafetySpec == Init /\ [][Next]_vars             \* faster, invariant-only

\* ============================================================================
\*                            SAFETY INVARIANTS
\* ============================================================================

\* TypeOK: stay inside the declared types.  Cheap, catches regressions.
TypeOK ==
    /\ notarVotes  \in [Validators -> SUBSET (Slots \X HashValues)]
    /\ skipVotes   \in [Validators -> SUBSET Slots]
    /\ finalVotes  \in [Validators -> SUBSET (Slots \X HashValues)]
    /\ observedNotar \in [Validators -> SUBSET (Slots \X HashValues)]
    /\ observedSkip  \in [Validators -> SUBSET Slots]
    /\ observedFinal \in [Validators -> SUBSET (Slots \X HashValues)]
    /\ reachedNotar \subseteq (Slots \X HashValues)
    /\ reachedSkip  \subseteq Slots
    /\ reachedFinal \subseteq (Slots \X HashValues)
    /\ frontier \in [Validators -> 0..(MaxSlot+1)]
    /\ knownCandidates \in
        [Validators -> SUBSET [slot : Slots,
                               hash : HashValues,
                               parentSlot : {GenesisSlot} \cup Slots,
                               parentHash : HashValues,
                               leader : Validators]]

\* Lemma 2.1: Final(s,h) and Skip(s) cannot both be reached.
FinalSkipExclusion ==
    \A s \in Slots : \A h \in HashValues :
        ~(<<s, h>> \in reachedFinal /\ s \in reachedSkip)

\* Lemma 2.2: at most one h with Notar(s,h) reached, per slot.
UniqueNotarization ==
    \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in reachedNotar /\ <<s, h2>> \in reachedNotar) => h1 = h2

\* Lemma 2.3: reached Final implies reached Notar.
FinalizationImpliesNotarization ==
    \A s \in Slots : \A h \in HashValues :
        <<s, h>> \in reachedFinal => <<s, h>> \in reachedNotar

\* Lemma 2.4: at most one h with Final(s,h) reached, per slot.
UniqueFinalization ==
    \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in reachedFinal /\ <<s, h2>> \in reachedFinal) => h1 = h2

\* §1.4 honest local rules.

HonestNotarUniqueness ==
    \A v \in HonestValidators : \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in notarVotes[v] /\ <<s, h2>> \in notarVotes[v]) => h1 = h2

HonestVoteConsistency ==
    \A v \in HonestValidators : \A s \in Slots :
        ~(\E h \in HashValues : <<s, h>> \in finalVotes[v]) \/ s \notin skipVotes[v]

HonestLeaderNoEquivocation ==
    \A v \in HonestValidators : \A s \in Slots :
        Cardinality({c \in knownCandidates[v] :
                        c.slot = s /\ c.leader = v /\ IsHonest(c.leader)}) <= 1

\* Theorem 2.6: chain ending at the earlier finalized slot is a prefix of
\* the chain ending at the later one.  WalkChainHashAt returns -1 when
\* the chain doesn't pass through (s1, _), which is the violation case.
ChainPrefixSafety ==
    \A s1 \in Slots : \A h1 \in HashValues :
        \A s2 \in Slots : \A h2 \in HashValues :
            (/\ <<s1, h1>> \in reachedFinal
             /\ <<s2, h2>> \in reachedFinal
             /\ s1 < s2
             /\ AnyCandidateExists(s2, h2)) =>
                WalkChainHashAt(s1, s2, h2) = h1

\*--------------------------------------------------------------------------
\* Legacy parent-check form of safety (kept for backward compatibility).
\* This follows directly from Lemma 2.1 (FinalSkipExclusion).
\*--------------------------------------------------------------------------
SafetyInvariant ==
    \A s1 \in Slots : \A h1 \in HashValues :
        \A s2 \in Slots : \A h2 \in HashValues :
            (/\ <<s1, h1>> \in reachedFinal
             /\ <<s2, h2>> \in reachedFinal
             /\ s1 < s2
             /\ AnyCandidateExists(s2, h2)) =>
                LET c2 == AnyKnownCandidate(s2, h2)
                IN ~(c2.parentSlot < s1 /\ s1 \in reachedSkip)

\*--------------------------------------------------------------------------
\* Corollary 2.7 (Consistency)
\* "Any two honest validators' output logs are such that one is a prefix
\*  of the other."
\*--------------------------------------------------------------------------
OutputLogConsistency ==
    \A v1, v2 \in HonestValidators :
        \A s1 \in Slots : \A h1 \in HashValues :
            \A s2 \in Slots : \A h2 \in HashValues :
                (/\ <<s1, h1>> \in observedFinal[v1]
                 /\ <<s2, h2>> \in observedFinal[v2]
                 /\ s1 = s2) => h1 = h2

\* Safety holds under 3f < W; trivially implied by ByzantineWeightBound,
\* but explicit so it shows up in the trace if someone configures past
\* the bound.
ByzantineFaultTolerance ==
    ByzantineWeight * 3 < TotalWeight => FinalSkipExclusion

\* ============================================================================
\*                            LIVENESS PROPERTIES
\* ============================================================================

\* Theorem 3.6: some slot eventually gets finalized.
EventualFinalization ==
    <>(\E s \in Slots : \E h \in HashValues : <<s, h>> \in reachedFinal)

\* Lemma 3.2: every slot is eventually cleared (frontier grows).
FrontierProgress ==
    \A v \in HonestValidators :
        \A s \in Slots :
            (frontier[v] = s) ~> (frontier[v] > s)

\* Theorem 3.7: one finalization is followed by another.
InfiniteFinalization ==
    \A s \in 0..(MaxSlot-1) :
        (\E h \in HashValues : <<s, h>> \in reachedFinal) ~>
        (\E s2 \in (s+1)..MaxSlot : \E h2 \in HashValues : <<s2, h2>> \in reachedFinal)

NoDeadlock == ENABLED(Next)

\* Compact box-form safety, handy as a single PROPERTY in cfgs.
SafetyUnderFaults ==
    [](FinalSkipExclusion /\ UniqueNotarization /\ UniqueFinalization)

SafetyUnderDrops ==
    [](FinalSkipExclusion /\ UniqueNotarization /\ UniqueFinalization)

\* ---- state-space constraints (TLC) -----------------------------------------

StateConstraint ==
    /\ \A v \in Validators : Cardinality(notarVotes[v]) <= MaxSlot + 1
    /\ \A v \in Validators : Cardinality(skipVotes[v]) <= MaxSlot + 1
    /\ \A v \in Validators : Cardinality(finalVotes[v]) <= MaxSlot + 1
    /\ Cardinality(messages) <= MaxMessages

\* ---- assumptions -----------------------------------------------------------

ASSUME ByzantineWeightBound == ByzantineWeight * 3 < TotalWeight   \* f < W/3
ASSUME PositiveWeights      == \A v \in Validators : Weight(v) > 0
ASSUME MaxSlot > 0
ASSUME LeaderWindowSize > 0

\* Quorum intersection.  Two quorums always share an honest validator;
\* this is the foundation of every safety lemma below.
ASSUME QuorumIntersectionLemma ==
    \A Q1, Q2 \in SUBSET Validators :
        (IsQuorum(Q1) /\ IsQuorum(Q2)) =>
            \E v \in (Q1 \cap Q2) : IsHonest(v)

\* ---- helpers for cfg substitution ------------------------------------------

UniformWeight1(v)   == 1
UniformWeight10(v)  == 10
HeavyWeight1(v)     == IF v = 1 THEN 3 ELSE 1
HeavyWeight1x5(v)   == IF v = 1 THEN 5 ELSE 1
NoByzantine         == {}
AlwaysValidSeq(chainState) == TRUE

====
