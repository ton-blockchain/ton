---- MODULE simplex ----
\******************************************************************************
\* TLA+ Specification of the Simplex Consensus Protocol (TON Catchain 2.0)
\*
\* Design decisions and abstractions:
\*   - Real time is abstracted away; timeouts are modeled as nondeterministic
\*     actions guarded by window activation. Fairness conditions ensure they
\*     eventually fire, matching the "finite timeout" guarantee of Rule 6.
\*   - The probabilistic post-GST network (Section 3.2) is modeled by
\*     nondeterministic message delivery + optional dropping. Fairness on
\*     delivery abstracts the "probability 1" delivery guarantee.
\*   - ValidSeq (Section 1.2) is an abstract constant operator. In the default
\*     model it is always TRUE, matching the spec's statement that "consensus
\*     does not depend on the internal structure of payloads."
\*   - Candidates are tracked per-validator (knownCandidates[v]) to model
\*     that a validator must *receive* a candidate before voting (Rule 4).
\*   - Leaders select bases using only locally provable state
\*     (observedNotar, observedSkip), not global oracle state (Rule 3).
\******************************************************************************

EXTENDS Integers, Sequences, FiniteSets, TLC, Naturals

\******************************************************************************
\*                      SYSTEM MODEL
\******************************************************************************

\* Number of validators (n in the spec)
CONSTANT NumValidators

\* Set of Byzantine validators (subset of 1..NumValidators)
\* At most f < W/3 total weight may be Byzantine
CONSTANT ByzantineSet

\* Weight of each validator: w_i (Section 1.1)
\* Declared as 1-arg operator for TLC config substitution
CONSTANT Weight(_)

\* Leader window size L (Section 1.3)
CONSTANT LeaderWindowSize

\******************************************************************************
\*                     MODEL CHECKING PARAMETERS
\******************************************************************************

\* Maximum slot number (bounds finite state space for TLC)
CONSTANT MaxSlot

\* Maximum messages in transit
CONSTANT MaxMessages

\* Network failure toggle: adversarial phase allows arbitrary message drops
CONSTANT EnableMessageDrop

\* Byzantine behavior toggles
CONSTANT EnableByzantineEquivocation
CONSTANT EnableByzantineWithholding

\******************************************************************************
\*     LEDGER VALIDITY (ValidSeq abstraction)
\*
\* ValidSeq : D* -> {true, false} with:
\*   1. Genesis validity: ValidSeq(empty) = true
\*   2. Extendability: for every valid seq, an extension exists
\*
\* "Consensus does not depend on the internal structure of payloads
\*  beyond these properties." —simplex.md §1.2
\*
\* We abstract ValidSeq as always TRUE. Override in cfg for payload checks.
\******************************************************************************

CONSTANT ValidSeq(_)

\******************************************************************************
\*                     DERIVED CONSTANTS
\******************************************************************************

Validators == 1..NumValidators
HonestValidators == Validators \ ByzantineSet

\* W = sum of all weights (Section 1.1)
TotalWeight == LET Sum[S \in SUBSET Validators] ==
                   IF S = {} THEN 0
                   ELSE LET v == CHOOSE x \in S : TRUE
                        IN Weight(v) + Sum[S \ {v}]
               IN Sum[Validators]

\* q = floor(2W/3) + 1 (Section 1.1)
QuorumThreshold == (2 * TotalWeight) \div 3 + 1

\* f = total Byzantine weight
ByzantineWeight == LET Sum[S \in SUBSET Validators] ==
                       IF S = {} THEN 0
                       ELSE LET v == CHOOSE x \in S : TRUE
                            IN Weight(v) + Sum[S \ {v}]
                   IN Sum[ByzantineSet]

Slots == 0..MaxSlot
HashValues == Validators \cup {0}

\* Genesis: Notar(-1, 0) and Final(-1, 0) are implicitly reached (Section 1.4)
GenesisSlot == -1
GenesisHash == 0

\******************************************************************************
\*                   PROTOCOL OBJECTS
\******************************************************************************

\* Leader rotation: window k has leader v_{(k mod n)+1}
\* Window k covers slots [kL, (k+1)L)
LeaderOf(slot) == ((slot \div LeaderWindowSize) % NumValidators) + 1
WindowOf(slot) == slot \div LeaderWindowSize
FirstSlotInWindow(k) == k * LeaderWindowSize
LastSlotInWindow(k)  == (k + 1) * LeaderWindowSize - 1

IsHonest(v) == v \notin ByzantineSet

\* Weight of a set of validators
WeightOf(S) == LET Sum[T \in SUBSET Validators] ==
                   IF T = {} THEN 0
                   ELSE LET v == CHOOSE x \in T : TRUE
                        IN Weight(v) + Sum[T \ {v}]
               IN Sum[S]

IsQuorum(S) == WeightOf(S) >= QuorumThreshold

\******************************************************************************
\*                        STATE VARIABLES
\*
\* Organized by the concepts they model from the spec.
\******************************************************************************

VARIABLES
    \* --- Section 1.4: Per-validator voting state ---
    \* Each honest validator tracks which votes it has cast.

    notarVotes,         \* notarVotes[v] : set of <<slot, hash>>
                        \* v has cast a vote for Notar(slot, hash)

    skipVotes,          \* skipVotes[v] : set of slot
                        \* v has cast a vote for Skip(slot)

    finalVotes,         \* finalVotes[v] : set of <<slot, hash>>
                        \* v has cast a vote for Final(slot, hash)

    \* --- Section 1.3 (Certificates): Per-validator observed certificates ---
    \* "S is observed on validator v when v receives enough votes to
    \*  construct a certificate for S" —simplex.md §1.3

    observedNotar,      \* observedNotar[v] : set of <<slot, hash>>
    observedSkip,       \* observedSkip[v] : set of slot
    observedFinal,      \* observedFinal[v] : set of <<slot, hash>>

    \* --- Section 1.3 (Certificates): Global reached state ---
    \* "S is reached if a certificate can be produced by an oracle that
    \*  knows all cast votes." —simplex.md §1.3

    reachedNotar,       \* set of <<slot, hash>>
    reachedSkip,        \* set of slot
    reachedFinal,       \* set of <<slot, hash>>

    \* --- Rule 1 (Frontier tracking) ---
    \* "F_v(t) is the smallest slot that is not cleared for v." —§3.1
    frontier,           \* frontier[v] : Nat

    \* --- Rule 2 (Candidate resolution): Per-validator candidate knowledge ---
    \* "Honest validators store candidates they have voted Notar for."
    \* A validator must *receive* a candidate before it can vote on it.
    knownCandidates,    \* knownCandidates[v] : set of candidate records
                        \* each record: [slot, hash, parentSlot, parentHash, leader]

    \* --- Network (Section 3.2) ---
    messages,           \* set of message records in transit
    droppedMessages     \* auxiliary: dropped messages (for debugging)

vars == <<notarVotes, skipVotes, finalVotes,
          observedNotar, observedSkip, observedFinal,
          reachedNotar, reachedSkip, reachedFinal,
          frontier, knownCandidates,
          messages, droppedMessages>>

\******************************************************************************
\*                       MESSAGE TYPES
\******************************************************************************

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

\* Rule 2: candidate resolution request/reply
CandidateRequestMsg(src, dst, slot, hash) ==
    [type |-> "candidateReq", src |-> src, dst |-> dst, slot |-> slot, hash |-> hash]

CandidateReplyMsg(src, dst, slot, hash, parentSlot, parentHash) ==
    [type |-> "candidateReply", src |-> src, dst |-> dst,
     slot |-> slot, hash |-> hash,
     parentSlot |-> parentSlot, parentHash |-> parentHash]

\* Rule 8: standstill broadcast bundles all data in one message
StandstillMsg(src, dst) ==
    [type |-> "standstill", src |-> src, dst |-> dst]

\******************************************************************************
\*                       INITIAL STATE
\******************************************************************************

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

\******************************************************************************
\*      CERTIFICATE FORMATION
\*
\* "A certificate for S is a set of votes for S from distinct validators
\*  with total weight >= q." —simplex.md §1.3
\******************************************************************************

NotarVotersFor(slot, hash)  == {v \in Validators : <<slot, hash>> \in notarVotes[v]}
SkipVotersFor(slot)          == {v \in Validators : slot \in skipVotes[v]}
FinalVotersFor(slot, hash)  == {v \in Validators : <<slot, hash>> \in finalVotes[v]}

NotarReached(slot, hash) == WeightOf(NotarVotersFor(slot, hash)) >= QuorumThreshold
SkipReached(slot)        == WeightOf(SkipVotersFor(slot))         >= QuorumThreshold
FinalReached(slot, hash) == WeightOf(FinalVotersFor(slot, hash)) >= QuorumThreshold

\******************************************************************************
\*      CANDIDATE KNOWLEDGE HELPERS
\*
\* Unlike the previous version which used a global candidates array,
\* this version tracks per-validator knowledge. A validator can only
\* vote on candidates it has received (Rule 4: "Upon receiving a candidate").
\******************************************************************************

\* Check if validator v knows a candidate at (slot, hash)
ValidatorKnowsCandidate(v, slot, hash) ==
    \E c \in knownCandidates[v] : c.slot = slot /\ c.hash = hash

\* Get the candidate record known to v
GetKnownCandidate(v, slot, hash) ==
    CHOOSE c \in knownCandidates[v] : c.slot = slot /\ c.hash = hash

\* Check if any validator knows a candidate at (slot, hash) — for invariant checking
AnyCandidateExists(slot, hash) ==
    \E v \in Validators : ValidatorKnowsCandidate(v, slot, hash)

\* Get any known candidate record — for invariant checking
AnyKnownCandidate(slot, hash) ==
    LET v == CHOOSE v \in Validators : ValidatorKnowsCandidate(v, slot, hash)
    IN GetKnownCandidate(v, slot, hash)

\******************************************************************************
\*   CHAIN VALIDITY
\*
\* "A candidate (s,h) is called valid if there exists a chain ending at
\*  (s,h) with a state that is valid." —simplex.md §1.3
\*
\* Chain validity requires:
\*   - First candidate has genesis parent (-1, 0)
\*   - Each subsequent candidate references the previous one
\*   - ValidSeq holds for the chain state
\*
\* For notarization, Rule 4 requires the chain to satisfy:
\*   - Parent Notar is reached
\*   - Intermediate slots are skipped
\*   - Candidate is valid
\******************************************************************************

\* Check chain validity from validator v's perspective
\* v can prove a candidate is valid if it knows the chain
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

\******************************************************************************
\*     WINDOW ACTIVATION (Rule 1 helper)
\*
\* "When F_v crosses a window boundary kL, window k becomes active for v."
\******************************************************************************

WindowActiveFor(v, k) == frontier[v] >= FirstSlotInWindow(k)

\******************************************************************************
\*     RULE 1 (Frontier tracking)
\*
\* "A slot s is cleared for v if v has observed Notar(s,·), Skip(s),
\*  or Final(s',·) for some s' >= s." —simplex.md §3.1
\******************************************************************************

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

\******************************************************************************
\*     RULE 2 (Candidate resolution)
\*
\* "Validator v can obtain a notarized candidate identified by (s,h) by
\*  requesting it from a uniformly random peer and retrying after an
\*  exponentially increasing timeout." —simplex.md §3.1
\*
\* Modeled as: v can request a candidate, and any validator that has it
\* can reply. The exponential backoff is abstracted by nondeterminism.
\******************************************************************************

\* v requests candidate (slot, hash) from peer dst
RequestCandidate(v, dst, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    /\ ~ValidatorKnowsCandidate(v, slot, hash)
    \* v must know the candidate exists (has seen a notar cert for it)
    /\ <<slot, hash>> \in observedNotar[v]
    /\ messages' = messages \cup {CandidateRequestMsg(v, dst, slot, hash)}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\* v replies to a candidate request (only if v has the candidate stored)
\* "When v receives such a request for a candidate it has stored, it replies."
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

\******************************************************************************
\*     RULE 3 (Leader duty)
\*
\* "When window k becomes active and v is its leader, v chooses any base
\*  (s_p, h_p) for which it can prove all of the following:
\*    1. s_p < kL,
\*    2. Notar(s_p, h_p) is reached,
\*    3. Skip(s') is reached for every s_p < s' < kL."
\*
\* KEY: The leader uses only what it can LOCALLY prove, i.e., its own
\* observedNotar and observedSkip, not the global oracle state.
\******************************************************************************

ProposeCandidate(leader, slot) ==
    /\ slot \in Slots
    /\ slot <= MaxSlot
    /\ LeaderOf(slot) = leader
    /\ IsHonest(leader)
    \* Window must be active for the leader (Rule 3: "when window k becomes active")
    /\ WindowActiveFor(leader, WindowOf(slot))
    \* Leader selects a base using LOCAL proof (observedNotar, observedSkip)
    /\ \E parentSlot \in {GenesisSlot} \cup (0..(slot-1)) :
        \E parentHash \in HashValues :
            \* Condition 1: s_p < kL (parent before this window)
            /\ parentSlot < FirstSlotInWindow(WindowOf(slot))
            \* Condition 2: Leader can LOCALLY prove Notar(s_p, h_p) is reached
            /\ (parentSlot = GenesisSlot \/
                <<parentSlot, parentHash>> \in observedNotar[leader])
            \* Condition 3: Leader can LOCALLY prove Skip(s') for all intermediate
            /\ \A s \in (parentSlot+1)..(slot-1) : s \in observedSkip[leader]
            \* Honest leader produces only one candidate per slot
            /\ ~(\E c \in knownCandidates[leader] :
                    c.slot = slot /\ c.leader = leader)
            \* ValidSeq check: the chain state is valid (§1.2)
            \* Abstracted: ValidSeq is assumed satisfiable (Extendability)
            /\ LET hash == leader
                   cand == [slot |-> slot, hash |-> hash,
                            parentSlot |-> parentSlot,
                            parentHash |-> parentHash,
                            leader |-> leader]
               IN
                \* Leader stores its own candidate (Rule 2: "store candidates")
                /\ knownCandidates' = [knownCandidates EXCEPT
                    ![leader] = @ \cup {cand}]
                \* Broadcast candidate to all validators (Rule 3)
                /\ messages' = messages \cup
                    {CandidateMsg(leader, dst, slot, hash, parentSlot, parentHash) :
                     dst \in Validators \ {leader}}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>

\******************************************************************************
\*     RULE 4 (Notarize)
\*
\* "Upon receiving a candidate (s,d,s_p,h_p,σ) identified by (s,h),
\*  validator v ... votes Notar(s,h) and broadcasts the vote as soon as
\*  it can prove all of the following:
\*    1. v has not previously voted Notar(s,·) for any candidate at slot s.
\*    2. Notar(s_p, h_p) is reached.
\*    3. For every slot s' with s_p < s' < s, Skip(s') is reached.
\*    4. Candidate (s,h) is valid."
\*
\* Conditions 2–4 are checked using v's LOCAL knowledge (observedNotar,
\* observedSkip, knownCandidates).
\******************************************************************************

VoteNotarize(v, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    \* Rule 4.1: No prior Notar vote at this slot for a different hash
    /\ ~(\E h2 \in HashValues : h2 /= hash /\ <<slot, h2>> \in notarVotes[v])
    \* Not already voted for this exact candidate
    /\ <<slot, hash>> \notin notarVotes[v]
    \* "Upon receiving a candidate" — v must have the candidate locally
    /\ ValidatorKnowsCandidate(v, slot, hash)
    /\ LET c == GetKnownCandidate(v, slot, hash)
       IN
        \* Rule 4.2: v can LOCALLY prove parent notarization is reached
        /\ (c.parentSlot = GenesisSlot \/
            <<c.parentSlot, c.parentHash>> \in observedNotar[v])
        \* Rule 4.3: v can LOCALLY prove all intermediate slots are skipped
        /\ \A s \in (c.parentSlot+1)..(slot-1) : s \in observedSkip[v]
        \* Rule 4.4: Candidate (s,h) is valid
        \* Abstracted: ValidSeq always TRUE; chain structure checked above
    \* Cast the vote
    /\ notarVotes' = [notarVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
    \* "Honest validators store candidates they have voted Notar for" (Rule 2)
    \* (Already stored since we checked ValidatorKnowsCandidate above)
    \* Broadcast the vote to all validators
    /\ messages' = messages \cup
        {NotarVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
    \* Update global reached state (oracle)
    /\ LET newVoters == NotarVotersFor(slot, hash) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedNotar' = reachedNotar \cup {<<slot, hash>>}
          ELSE reachedNotar' = reachedNotar
    /\ UNCHANGED <<skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedSkip, reachedFinal, frontier, droppedMessages>>

\******************************************************************************
\*     RULE 5 (Finalize)
\*
\* "Validator v votes Final(s,h) and broadcasts the vote as soon as
\*  it can prove all of the following:
\*    1. v has voted Notar(s,h),
\*    2. Notar(s,h) is reached,
\*    3. v has not voted Skip(s)."
\******************************************************************************

VoteFinalize(v, slot, hash) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    \* Rule 5.1: v has voted Notar(slot, hash)
    /\ <<slot, hash>> \in notarVotes[v]
    \* Rule 5.2: v can prove Notar(slot, hash) is reached (observed)
    /\ <<slot, hash>> \in observedNotar[v]
    \* Rule 5.3: v has not voted Skip(slot)
    /\ slot \notin skipVotes[v]
    \* Not already voted final for this
    /\ <<slot, hash>> \notin finalVotes[v]
    \* Cast the vote
    /\ finalVotes' = [finalVotes EXCEPT ![v] = @ \cup {<<slot, hash>>}]
    \* Broadcast the vote
    /\ messages' = messages \cup
        {FinalVoteMsg(v, dst, slot, hash) : dst \in Validators \ {v}}
    \* Update global reached state
    /\ LET newVoters == FinalVotersFor(slot, hash) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedFinal' = reachedFinal \cup {<<slot, hash>>}
          ELSE reachedFinal' = reachedFinal
    /\ UNCHANGED <<notarVotes, skipVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, frontier, droppedMessages>>

\******************************************************************************
\*     RULE 6 (Skip)
\*
\* "For each window k, after the window becomes active for v, and for
\*  each slot s in the window, there is a finite timeout T_skip(s) >= T_0
\*  after which v votes Skip(s) (unless it has already voted Final(s,·)),
\*  and broadcasts the vote."
\*
\* Timeout modeling: "Let k* be the window containing the largest slot s*
\*  such that v can prove Final(s*,·) is reached at the moment window k
\*  becomes active. Then T_skip(s) >= T_0 · α^{k-k*-1}."
\*
\* In TLA+, real time is abstracted. The skip action is nondeterministically
\* enabled when the window is active. The exponential growth constraint
\* means that in a real execution, the skip fires later when there is a
\* large gap since the last finalization. Fairness ensures it eventually
\* fires, matching the "finite timeout" guarantee.
\*
\* The ordering between skip and finalize is enforced by:
\*   - Skip requires: v has not voted Final(s,·)
\*   - Final requires: v has not voted Skip(s)
\* These mutual exclusions encode the critical safety constraint from §1.4.
\******************************************************************************

VoteSkip(v, slot) ==
    /\ IsHonest(v)
    /\ slot \in Slots
    \* Section 1.4: "v has not cast a vote for any Final(s,h)"
    /\ ~(\E h \in HashValues : <<slot, h>> \in finalVotes[v])
    \* Not already voted skip for this slot
    /\ slot \notin skipVotes[v]
    \* Rule 6: Window containing slot must be active for v
    /\ WindowActiveFor(v, WindowOf(slot))
    \* Cast the vote
    /\ skipVotes' = [skipVotes EXCEPT ![v] = @ \cup {slot}]
    \* Broadcast
    /\ messages' = messages \cup
        {SkipVoteMsg(v, dst, slot) : dst \in Validators \ {v}}
    \* Update global reached state
    /\ LET newVoters == SkipVotersFor(slot) \cup {v}
       IN IF WeightOf(newVoters) >= QuorumThreshold
          THEN reachedSkip' = reachedSkip \cup {slot}
          ELSE reachedSkip' = reachedSkip
    /\ UNCHANGED <<notarVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedFinal, frontier, droppedMessages>>

\******************************************************************************
\*     RULE 7 (Certificate formation and rebroadcast)
\*
\* "When v has received votes for a statement S with total weight at
\*  least q, it forms the corresponding certificate. Upon forming or
\*  receiving any certificate, v broadcasts it."
\******************************************************************************

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

\******************************************************************************
\*     RULE 8 (Standstill resolution)
\*
\* "When v has not observed any new finalized blocks since time t_0,
\*  the j-th standstill-resolution attempt occurs at time t_0 + (j+1)T_s.
\*  Each standstill-resolution attempt is a broadcast that sends:
\*    1. the certificate for Final(s,·) with largest slot s that v has observed,
\*    2. all certificates v holds for slots > s,
\*    3. all votes cast by v for slots > s."
\*
\* Modeled as: if v has certificates or votes above its latest observed
\* finalization, it can (nondeterministically) send them to any peer.
\* Fairness ensures this eventually happens.
\******************************************************************************

\* Largest finalized slot observed by v, or -1
LatestObservedFinalSlot(v) ==
    IF observedFinal[v] = {} THEN GenesisSlot
    ELSE LET maxSlot == CHOOSE s \in {p[1] : p \in observedFinal[v]} :
              \A s2 \in {p[1] : p \in observedFinal[v]} : s >= s2
         IN maxSlot

StandstillBroadcast(v) ==
    /\ IsHonest(v)
    /\ LET sF == LatestObservedFinalSlot(v)
       IN \E dst \in Validators \ {v} :
           \* Send all certificates and votes for slots > sF
           \* Modeled as individual cert/vote messages (TLA+ abstraction)
           \/ \* Send a final cert above sF
              /\ \E sh \in observedFinal[v] : sh[1] >= sF
              /\ \E sh \in observedFinal[v] :
                    messages' = messages \cup
                        {FinalCertMsg(v, dst, sh[1], sh[2])}
              /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                   knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal,
                   frontier, droppedMessages>>
           \/ \* Send a notar cert above sF
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
           \/ \* Send a skip cert above sF
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
           \/ \* Send own votes above sF
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
           \/ \* Send own skip votes above sF
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
           \/ \* Send own final votes above sF
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

\******************************************************************************
\*     BYZANTINE BEHAVIOR
\*
\* Byzantine validators may violate any honest rule constraint.
\* They can: vote multiple times for conflicting statements, propose
\* conflicting candidates (equivocation), withhold votes, etc.
\******************************************************************************

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

\******************************************************************************
\*     NETWORK MODEL
\*
\* "Adversarial phase (t < T_GST): The adversary controls message delivery…
\*  it may delay, reorder, or drop messages arbitrarily.
\*  Good phase (t >= T_GST): Each message…is delivered within time δ
\*  with probability 1-r and lost otherwise."
\*
\* Modeled as:
\*   - DeliverMessage: nondeterministically delivers any message in transit.
\*   - DropMessage: enabled when EnableMessageDrop=TRUE (adversarial phase).
\*   - Fairness on delivery abstracts the post-GST probabilistic guarantee:
\*     with probability 1, messages are eventually delivered.
\******************************************************************************

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
            \* Deliver candidate: store in recipient's knownCandidates
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
            \* Rule 2: deliver resolved candidate to requester
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
            \* Route to ReplyCandidate (just remove from messages here)
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

        [] msg.type = "standstill" ->
            \* Standstill messages are handled by cert/vote delivery
            /\ UNCHANGED <<notarVotes, skipVotes, finalVotes,
                           knownCandidates,
                           observedNotar, observedSkip, observedFinal,
                           reachedNotar, reachedSkip, reachedFinal,
                           frontier, droppedMessages>>

\* Adversarial phase: messages can be dropped
DropMessage(msg) ==
    /\ EnableMessageDrop
    /\ msg \in messages
    /\ messages' = messages \ {msg}
    /\ droppedMessages' = droppedMessages \cup {msg}
    /\ UNCHANGED <<notarVotes, skipVotes, finalVotes, knownCandidates,
                   observedNotar, observedSkip, observedFinal,
                   reachedNotar, reachedSkip, reachedFinal, frontier>>

\******************************************************************************
\*                      NEXT STATE RELATION
\******************************************************************************

Next ==
    \* Rule 3: Leader proposes candidate
    \/ \E v \in HonestValidators : \E s \in Slots :
        /\ LeaderOf(s) = v
        /\ ProposeCandidate(v, s)
    \* Byzantine leader equivocation
    \/ \E v \in ByzantineSet : \E s \in Slots :
        ByzantinePropose(v, s)
    \* Rule 4: Notarize
    \/ \E v \in HonestValidators : \E s \in Slots : \E h \in HashValues :
        VoteNotarize(v, s, h)
    \* Rule 5: Finalize
    \/ \E v \in HonestValidators : \E s \in Slots : \E h \in HashValues :
        VoteFinalize(v, s, h)
    \* Rule 6: Skip (timeout)
    \/ \E v \in HonestValidators : \E s \in Slots :
        VoteSkip(v, s)
    \* Byzantine arbitrary voting
    \/ \E v \in ByzantineSet :
        ByzantineVote(v)
    \* Network: message delivery
    \/ \E msg \in messages :
        DeliverMessage(msg)
    \* Network: message drop (adversarial phase)
    \/ \E msg \in messages :
        DropMessage(msg)
    \* Rule 1: Update frontier
    \/ \E v \in HonestValidators :
        UpdateFrontier(v)
    \* Rule 2: Candidate resolution (request)
    \/ \E v \in HonestValidators : \E dst \in Validators :
        \E s \in Slots : \E h \in HashValues :
            RequestCandidate(v, dst, s, h)
    \* Rule 2: Candidate resolution (reply)
    \/ \E v \in HonestValidators : \E msg \in messages :
        ReplyCandidate(v, msg)
    \* Rule 7: Certificate rebroadcast
    \/ \E v \in Validators : \E s \in Slots : \E h \in HashValues :
        RebroadcastNotarCert(v, s, h)
    \/ \E v \in Validators : \E s \in Slots :
        RebroadcastSkipCert(v, s)
    \/ \E v \in Validators : \E s \in Slots : \E h \in HashValues :
        RebroadcastFinalCert(v, s, h)
    \* Rule 8: Standstill resolution
    \/ \E v \in HonestValidators :
        StandstillBroadcast(v)

\******************************************************************************
\*        FAIRNESS (Section 3.1 + 3.2)
\*
\* Safety is unconditional (no fairness needed).
\* Liveness requires:
\*   - Honest validators eventually act when their preconditions are met
\*     (WF = weak fairness: "if continuously enabled, eventually taken")
\*   - Messages are eventually delivered (SF = strong fairness: "if
\*     repeatedly enabled, eventually taken" — models the probabilistic
\*     post-GST guarantee that each delivery attempt succeeds w.p. > 0)
\*   - Standstill broadcasts eventually reach peers (Rule 8 + Assumption 9)
\******************************************************************************

Fairness ==
    \* Honest validators eventually vote when preconditions are met
    /\ \A v \in HonestValidators : \A s \in Slots : \A h \in HashValues :
        WF_vars(VoteNotarize(v, s, h))
    /\ \A v \in HonestValidators : \A s \in Slots : \A h \in HashValues :
        WF_vars(VoteFinalize(v, s, h))
    /\ \A v \in HonestValidators : \A s \in Slots :
        WF_vars(VoteSkip(v, s))
    \* Rule 1: Frontier advances when possible
    /\ \A v \in HonestValidators :
        WF_vars(UpdateFrontier(v))
    \* Rule 3: Leaders propose when window becomes active
    /\ \A v \in HonestValidators : \A s \in Slots :
        WF_vars(ProposeCandidate(v, s))
    \* Section 3.2: Messages are eventually delivered
    /\ SF_vars(\E msg \in messages : DeliverMessage(msg))
    \* Rule 7: Certificates are rebroadcast
    /\ \A v \in Validators : \A s \in Slots : \A h \in HashValues :
        WF_vars(RebroadcastNotarCert(v, s, h))
    /\ \A v \in Validators : \A s \in Slots :
        WF_vars(RebroadcastSkipCert(v, s))
    /\ \A v \in Validators : \A s \in Slots : \A h \in HashValues :
        WF_vars(RebroadcastFinalCert(v, s, h))

\* Full specification: Init, Next, Fairness
Spec == Init /\ [][Next]_vars /\ Fairness

\* Safety-only spec (no fairness, for faster invariant checking)
SafetySpec == Init /\ [][Next]_vars

\******************************************************************************
\*                   SAFETY INVARIANTS
\******************************************************************************

\*--------------------------------------------------------------------------
\* Lemma 2.1 (Final–Skip exclusion)
\* "For any slot s and hash h, Final(s,h) and Skip(s) cannot both be reached."
\*--------------------------------------------------------------------------
FinalSkipExclusion ==
    \A s \in Slots : \A h \in HashValues :
        ~(<<s, h>> \in reachedFinal /\ s \in reachedSkip)

\*--------------------------------------------------------------------------
\* Lemma 2.2 (Unique notarization)
\* "For any slot s, at most one h can have Notar(s,h) reached."
\*--------------------------------------------------------------------------
UniqueNotarization ==
    \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in reachedNotar /\ <<s, h2>> \in reachedNotar) => h1 = h2

\*--------------------------------------------------------------------------
\* Lemma 2.3 (Finalization implies notarization)
\* "If Final(s,h) is reached, then Notar(s,h) is reached."
\*--------------------------------------------------------------------------
FinalizationImpliesNotarization ==
    \A s \in Slots : \A h \in HashValues :
        <<s, h>> \in reachedFinal => <<s, h>> \in reachedNotar

\*--------------------------------------------------------------------------
\* Lemma 2.4 (Unique finalization)
\* "For any slot s, at most one h can have Final(s,h) reached."
\*--------------------------------------------------------------------------
UniqueFinalization ==
    \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in reachedFinal /\ <<s, h2>> \in reachedFinal) => h1 = h2

\*--------------------------------------------------------------------------
\* Section 1.4: Honest validator local uniqueness constraints
\* An honest validator never votes Notar for two different hashes at same slot
\*--------------------------------------------------------------------------
HonestNotarUniqueness ==
    \A v \in HonestValidators : \A s \in Slots : \A h1, h2 \in HashValues :
        (<<s, h1>> \in notarVotes[v] /\ <<s, h2>> \in notarVotes[v]) => h1 = h2

\*--------------------------------------------------------------------------
\* Section 1.4: Honest vote consistency
\* An honest validator never votes both Final(s,*) and Skip(s)
\*--------------------------------------------------------------------------
HonestVoteConsistency ==
    \A v \in HonestValidators : \A s \in Slots :
        ~(\E h \in HashValues : <<s, h>> \in finalVotes[v]) \/ s \notin skipVotes[v]

\*--------------------------------------------------------------------------
\* Theorem 2.6 (Safety)
\* "If Final(s_a,h_a) and Final(s_b,h_b) are both reached with s_a<=s_b,
\*  then the chain ending at (s_a,h_a) is a prefix of the chain ending at
\*  (s_b,h_b)."
\*
\* Checked as: if slot s1 is finalized and s2>s1 is also finalized, then
\* s1 is NOT skipped in the chain of s2 (contradicts Lemma 2.1).
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

\*--------------------------------------------------------------------------
\* Byzantine Fault Tolerance
\* Safety holds when f < W/3
\*--------------------------------------------------------------------------
ByzantineFaultTolerance ==
    ByzantineWeight * 3 < TotalWeight => FinalSkipExclusion

\******************************************************************************
\*                LIVENESS PROPERTIES
\******************************************************************************

\*--------------------------------------------------------------------------
\* Theorem 3.6: "With probability 1 some slot s > s_f is eventually finalized."
\* Under fairness and non-adversarial network (no drops), eventually a
\* slot is finalized.
\*--------------------------------------------------------------------------
EventualFinalization ==
    <>(\E s \in Slots : \E h \in HashValues : <<s, h>> \in reachedFinal)

\*--------------------------------------------------------------------------
\* Lemma 3.2: Every slot is eventually cleared; frontier -> infinity.
\*--------------------------------------------------------------------------
FrontierProgress ==
    \A v \in HonestValidators :
        \A s \in Slots :
            (frontier[v] = s) ~> (frontier[v] > s)

\*--------------------------------------------------------------------------
\* Theorem 3.7: "With probability 1, infinitely many slots are finalized."
\* If one slot is finalized, another later slot will be eventually.
\*--------------------------------------------------------------------------
InfiniteFinalization ==
    \A s \in 0..(MaxSlot-1) :
        (\E h \in HashValues : <<s, h>> \in reachedFinal) ~>
        (\E s2 \in (s+1)..MaxSlot : \E h2 \in HashValues : <<s2, h2>> \in reachedFinal)

\*--------------------------------------------------------------------------
\* No Deadlock: some action is always enabled
\*--------------------------------------------------------------------------
NoDeadlock == ENABLED(Next)

\******************************************************************************
\*        COMBINED TEMPORAL SAFETY PROPERTIES
\******************************************************************************

SafetyUnderFaults ==
    [](FinalSkipExclusion /\ UniqueNotarization /\ UniqueFinalization)

SafetyUnderDrops ==
    [](FinalSkipExclusion /\ UniqueNotarization /\ UniqueFinalization)

\******************************************************************************
\*                  MODEL CHECKING CONSTRAINTS
\******************************************************************************

StateConstraint ==
    /\ \A v \in Validators : Cardinality(notarVotes[v]) <= MaxSlot + 1
    /\ \A v \in Validators : Cardinality(skipVotes[v]) <= MaxSlot + 1
    /\ \A v \in Validators : Cardinality(finalVotes[v]) <= MaxSlot + 1
    /\ Cardinality(messages) <= MaxMessages

\******************************************************************************
\*                    ASSUMPTIONS
\******************************************************************************

\* Section 1.1: f < W/3
ASSUME ByzantineWeightBound == ByzantineWeight * 3 < TotalWeight

\* Section 1.1: positive weights
ASSUME PositiveWeights == \A v \in Validators : Weight(v) > 0

ASSUME MaxSlot > 0
ASSUME LeaderWindowSize > 0

\******************************************************************************
\*   WEIGHT/CONFIG OPERATOR DEFINITIONS FOR TLC SUBSTITUTION
\******************************************************************************

\* Uniform weight 1
UniformWeight1(v) == 1

\* Uniform weight 10
UniformWeight10(v) == 10

\* Validator 1 has weight 3, others 1
HeavyWeight1(v) == IF v = 1 THEN 3 ELSE 1

\* Validator 1 has weight 5, others 1
HeavyWeight1x5(v) == IF v = 1 THEN 5 ELSE 1

\* Empty set for ByzantineSet
NoByzantine == {}

\* ValidSeq always true (§1.2 abstraction)
AlwaysValidSeq(chainState) == TRUE

====
