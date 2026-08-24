package collator

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/xssnick/gton/service/validator/groups"
	"github.com/xssnick/gton/service/validator/msgpool"
	"github.com/xssnick/gton/service/validator/simplex"
)

const (
	diskCorpusEnvironment                = "GTON_BENCH_CORPUS_DIR"
	diskCorpusCandidateOutputEnvironment = "GTON_BENCH_CANDIDATE_OUT_DIR"
	diskCorpusAllowParallelEnvironment   = "GTON_BENCH_ALLOW_PARALLEL"
	diskCorpusFormat                     = "ton-collation-validation-fixture"
	diskCorpusVersion                    = uint32(1)
	diskCorpusWorkloadNameJetton         = "wallet-v5-jetton-transfer"
	diskCorpusWorkloadNameTransfer       = "wallet-v5-transfer"
	diskCorpusGlobalID                   = int32(-777)
	diskCorpusMetaName                   = "meta.json"
	diskCorpusSumName                    = "manifest.sha256"
	diskCorpusOutputBlockName            = "block.boc"
	diskCorpusOutputCollatedName         = "collated.boc"
	// The frozen v1 generator emits four cells per stored account plus 52
	// shard-state scaffolding cells. This is a parser ceiling, not permission
	// to raise the process-wide BOC limit or relax any other artifact.
	diskCorpusShardStateCellsPerAccount = uint64(4)
	diskCorpusShardStateFixedCells      = uint64(52)
)

// The disk corpus is a benchmark input, not a node storage format. Its cells
// are parsed independently from their exact BOCs and are never reconciled by
// representation hash: equal hashes do not imply equal cell occurrences.
type diskCorpusManifest struct {
	Format      string                    `json:"format"`
	Version     uint32                    `json:"version"`
	Workload    diskCorpusWorkload        `json:"workload"`
	Contract    diskCorpusContract        `json:"contract"`
	Header      diskCorpusHeader          `json:"header"`
	Previous    diskCorpusBlock           `json:"previous"`
	Masterchain diskCorpusBlock           `json:"masterchain"`
	Candidate   diskCorpusCandidate       `json:"candidate"`
	Externals   diskCorpusExternals       `json:"externals"`
	Files       map[string]diskCorpusFile `json:"files"`
	Expected    diskCorpusExpected        `json:"expected"`
}

type diskCorpusWorkload struct {
	Name      string `json:"name"`
	Version   uint32 `json:"version"`
	Accounts  uint32 `json:"accounts"`
	Transfers uint32 `json:"transfers"`
	Seed      string `json:"seed"`
}

type diskCorpusContract struct {
	Workchain            int32  `json:"workchain"`
	Shard                string `json:"shard"`
	PreviousCount        uint32 `json:"previous_count"`
	BeforeSplit          bool   `json:"before_split"`
	AfterSplit           bool   `json:"after_split"`
	AfterMerge           bool   `json:"after_merge"`
	InternalsCount       uint32 `json:"internals_count"`
	InternalsComplete    bool   `json:"internals_complete"`
	NeighborsCount       uint32 `json:"neighbors_count"`
	TopBlocksCount       uint32 `json:"top_blocks_count"`
	OutQueueSize         string `json:"out_queue_size"`
	FullCollatedData     bool   `json:"full_collated_data"`
	MaxExternalAttempts  uint32 `json:"max_external_attempts"`
	StorageStatCache     bool   `json:"storage_stat_cache"`
	QueueCleanupDeadline bool   `json:"queue_cleanup_deadline"`
	InternalMsgDeadline  bool   `json:"internal_msg_deadline"`
}

type diskCorpusHeader struct {
	GenUtime   uint32 `json:"gen_utime"`
	GenUtimeMS string `json:"gen_utime_ms"`
	RandSeed   string `json:"rand_seed"`
	CreatedBy  string `json:"created_by"`
}

type diskCorpusBlock struct {
	Workchain int32  `json:"workchain"`
	Shard     string `json:"shard"`
	SeqNo     uint32 `json:"seqno"`
	RootHash  string `json:"root_hash"`
	FileHash  string `json:"file_hash"`
}

type diskCorpusCandidate struct {
	Block                     diskCorpusBlock `json:"block"`
	CollatedFileHash          string          `json:"collated_file_hash"`
	PreloadedCollatedFileHash string          `json:"preloaded_collated_file_hash"`
	StateRootHash             string          `json:"state_root_hash"`
}

type diskCorpusExternals struct {
	Count   uint32 `json:"count"`
	Pattern string `json:"pattern"`
}

type diskCorpusFile struct {
	Path       string   `json:"path"`
	Size       string   `json:"size"`
	SHA256     string   `json:"sha256"`
	RootCount  uint32   `json:"root_count"`
	RootHashes []string `json:"root_hashes"`
}

type diskCorpusExpected struct {
	Transactions           uint32 `json:"transactions"`
	GasUsed                string `json:"gas_used"`
	BlockBytes             string `json:"block_bytes"`
	CollatedFullBytes      string `json:"collated_full_bytes"`
	CollatedPreloadedBytes string `json:"collated_preloaded_bytes"`
}

type diskCorpusArtifact struct {
	raw   []byte
	roots []*cell.Cell
}

type diskCorpus struct {
	request              ShardRequest
	fullCandidate        *Candidate
	preloadedCandidate   *Candidate
	expectedTransactions uint32
	expectedGas          uint64
}

var diskCorpusCandidateSink *Candidate

func requireDiskCorpusSingleCPU(tb testing.TB) {
	tb.Helper()
	if os.Getenv(diskCorpusAllowParallelEnvironment) == "1" {
		return
	}
	if got := runtime.GOMAXPROCS(0); got != 1 {
		tb.Fatalf("disk corpus headline requires GOMAXPROCS=1 (got %d); use -cpu=1 or set GOMAXPROCS=1,"+
			" or opt in with %s=1", got, diskCorpusAllowParallelEnvironment)
	}
}

// TestDiskCorpusCrossAcceptance is the correctness gate for benchmark input
// produced by the C++ reference fixture. It is intentionally absent from a
// normal test run: setting GTON_BENCH_CORPUS_DIR opts into reading the external
// corpus and requires both its full and resident-state sidecars to validate.
func TestDiskCorpusCrossAcceptance(t *testing.T) {
	corpus := loadDiskCorpus(t)
	ctx := context.Background()

	for _, variant := range []struct {
		name      string
		candidate *Candidate
	}{
		{name: "collated", candidate: corpus.fullCandidate},
		{name: "preloaded", candidate: corpus.preloadedCandidate},
	} {
		t.Run("reference/"+variant.name, func(t *testing.T) {
			semantics := NewSemanticVerifier(tvm.NewTVM())
			if err := validateDiskCorpusCandidate(ctx, corpus.request, variant.candidate, semantics); err != nil {
				t.Fatalf("verify reference %s candidate: %v", variant.name, err)
			}
		})
	}

	builder := NewBuilder(tvm.NewTVM(), SupportedSoftware())
	candidate, err := builder.BuildShard(ctx, corpus.request)
	if err != nil {
		t.Fatalf("collate disk corpus with gton: %v", err)
	}
	assertDiskCorpusCandidate(t, corpus, candidate)
	semantics := NewSemanticVerifier(tvm.NewTVM())
	if err = validateDiskCorpusCandidate(ctx, corpus.request, candidate, semantics); err != nil {
		t.Fatalf("verify gton candidate built from disk corpus: %v", err)
	}
	if out := os.Getenv(diskCorpusCandidateOutputEnvironment); out != "" {
		if err = exportDiskCorpusCandidate(os.Getenv(diskCorpusEnvironment), out, corpus.request, candidate); err != nil {
			t.Fatalf("export gton candidate: %v", err)
		}
		t.Logf("exported gton candidate to %s", out)
	}
}

func exportDiskCorpusCandidate(
	corpusDir string,
	outDir string,
	request ShardRequest,
	candidate *Candidate,
) error {
	if candidate == nil || len(candidate.BlockBOC) == 0 || len(candidate.CollatedData) == 0 {
		return errors.New("candidate block or full collated data is absent")
	}
	if err := validateDiskCorpusExportCandidate(request, candidate); err != nil {
		return err
	}

	corpusPath, err := filepath.EvalSymlinks(corpusDir)
	if err != nil {
		return fmt.Errorf("resolve corpus directory: %w", err)
	}
	corpusPath, err = filepath.Abs(corpusPath)
	if err != nil {
		return fmt.Errorf("make corpus directory absolute: %w", err)
	}
	outPath, err := filepath.Abs(outDir)
	if err != nil {
		return fmt.Errorf("make candidate output directory absolute: %w", err)
	}
	outParent, err := filepath.EvalSymlinks(filepath.Dir(outPath))
	if err != nil {
		return fmt.Errorf("resolve candidate output parent (it must already exist): %w", err)
	}
	outPath = filepath.Join(outParent, filepath.Base(outPath))
	if rel, relErr := filepath.Rel(corpusPath, outPath); relErr != nil {
		return fmt.Errorf("compare corpus and candidate output paths: %w", relErr)
	} else if rel == "." || (rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))) {
		return errors.New("candidate output directory must not be the corpus or reside inside it")
	}
	if _, err = os.Lstat(outPath); err == nil {
		return fmt.Errorf("candidate output directory already exists: %s", outPath)
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("inspect candidate output directory: %w", err)
	}

	if err = os.Mkdir(outPath, 0o755); err != nil {
		return fmt.Errorf("reserve candidate output directory: %w", err)
	}
	complete := false
	defer func() {
		if !complete {
			_ = os.Remove(filepath.Join(outPath, diskCorpusOutputBlockName))
			_ = os.Remove(filepath.Join(outPath, diskCorpusOutputCollatedName))
			_ = os.Remove(outPath)
		}
	}()

	if err = writeDiskCorpusCandidateFile(outPath, diskCorpusOutputBlockName, candidate.BlockBOC); err != nil {
		return err
	}
	if err = writeDiskCorpusCandidateFile(outPath, diskCorpusOutputCollatedName, candidate.CollatedData); err != nil {
		return err
	}
	complete = true

	return nil
}

func validateDiskCorpusExportCandidate(request ShardRequest, candidate *Candidate) error {
	blockFileHash := sha256.Sum256(candidate.BlockBOC)
	if !bytes.Equal(candidate.ID.FileHash, blockFileHash[:]) {
		return errors.New("export candidate block file hash differs from its raw BOC")
	}
	collatedFileHash := sha256.Sum256(candidate.CollatedData)
	if collatedFileHash != candidate.CollatedFileHash {
		return errors.New("export candidate collated file hash differs from its raw BOC")
	}

	roots, err := cell.FromBOCMultiRoot(candidate.BlockBOC)
	if err != nil {
		return fmt.Errorf("decode export candidate block: %w", err)
	}
	if len(roots) != 1 {
		return fmt.Errorf("export candidate block BOC has %d roots, want 1", len(roots))
	}
	root := roots[0]
	if !bytes.Equal(candidate.ID.RootHash, root.Hash()) {
		return errors.New("export candidate root hash differs from its raw BOC")
	}
	var block tlb.Block
	if err = parseExact(&block, root); err != nil {
		return fmt.Errorf("decode exact export candidate block: %w", err)
	}
	if err = verifyExactBlockParts(root, &block); err != nil {
		return fmt.Errorf("verify exact export candidate block: %w", err)
	}
	if err = verifyHeaderAndID(&block.BlockInfo, candidate); err != nil {
		return fmt.Errorf("verify export candidate id: %w", err)
	}
	if candidate.ID.Workchain != request.Shard.Workchain || candidate.ID.Shard != request.Shard.Shard ||
		candidate.ID.SeqNo != request.Previous.ID.SeqNo+1 {
		return errors.New("export candidate id differs from the corpus request")
	}
	if block.GlobalID != diskCorpusGlobalID || block.BlockInfo.GenUtime != request.Header.GenUtime ||
		block.BlockInfo.BeforeSplit != request.BeforeSplit || block.BlockInfo.AfterSplit || block.BlockInfo.AfterMerge {
		return errors.New("export candidate header differs from the corpus request")
	}
	if block.Extra == nil || candidate.CreatedBy != request.CreatedBy ||
		!bytes.Equal(block.Extra.CreatedBy, request.CreatedBy[:]) ||
		!bytes.Equal(block.Extra.RandSeed, request.RandSeed[:]) {
		return errors.New("export candidate creator or random seed differs from the corpus request")
	}

	collated, err := verifyCollatedData(candidate, nil, request.Header.GenUtime)
	if err != nil {
		return fmt.Errorf("verify export candidate collated data: %w", err)
	}
	if !collated.full {
		return errors.New("export candidate does not contain full collated data")
	}
	if collated.genUtimeMS != request.Header.GenUtimeMS {
		return fmt.Errorf(
			"export candidate generation time = %d ms, want %d ms",
			collated.genUtimeMS,
			request.Header.GenUtimeMS,
		)
	}

	return nil
}

func writeDiskCorpusCandidateFile(dir string, name string, data []byte) error {
	temporary, err := os.CreateTemp(dir, "."+name+".tmp-")
	if err != nil {
		return fmt.Errorf("create staged %s: %w", name, err)
	}
	temporaryName := temporary.Name()
	defer os.Remove(temporaryName)

	if err = temporary.Chmod(0o644); err != nil {
		_ = temporary.Close()
		return fmt.Errorf("set staged %s permissions: %w", name, err)
	}
	written, err := temporary.Write(data)
	if err != nil {
		_ = temporary.Close()
		return fmt.Errorf("write staged %s: %w", name, err)
	}
	if written != len(data) {
		_ = temporary.Close()
		return fmt.Errorf("write staged %s: wrote %d bytes, want %d", name, written, len(data))
	}
	if err = temporary.Sync(); err != nil {
		_ = temporary.Close()
		return fmt.Errorf("sync staged %s: %w", name, err)
	}
	if err = temporary.Close(); err != nil {
		return fmt.Errorf("close staged %s: %w", name, err)
	}
	if err = os.Rename(temporaryName, filepath.Join(dir, name)); err != nil {
		return fmt.Errorf("publish %s atomically: %w", name, err)
	}

	return nil
}

// BenchmarkCollateDiskCorpus times only Builder.BuildShard. Manifest reads,
// checksums, BOC parsing, configuration/group derivation and the correctness
// preflight are completed before ResetTimer.
func BenchmarkCollateDiskCorpus(b *testing.B) {
	requireDiskCorpusSingleCPU(b)
	corpus := loadDiskCorpus(b)
	builder := NewBuilder(tvm.NewTVM(), SupportedSoftware())
	ctx := context.Background()

	candidate, err := builder.BuildShard(ctx, corpus.request)
	if err != nil {
		b.Fatalf("collation preflight: %v", err)
	}
	assertDiskCorpusCandidate(b, corpus, candidate)

	b.ReportAllocs()
	var measured *Candidate
	b.ResetTimer()
	for b.Loop() {
		var buildErr error
		measured, buildErr = builder.BuildShard(ctx, corpus.request)
		if buildErr != nil {
			b.Fatal(buildErr)
		}
	}
	b.StopTimer()
	assertDiskCorpusCandidate(b, corpus, measured)
	diskCorpusCandidateSink = measured
	b.ReportMetric(float64(candidate.Stats.Transactions), "tx/block")
	b.ReportMetric(float64(candidate.Stats.GasUsed), "gas/block")
	b.ReportMetric(float64(len(candidate.BlockBOC)), "blockB")
	b.ReportMetric(float64(len(candidate.CollatedData)), "collatedB")
}

// BenchmarkValidateDiskCorpus is the cross-implementation validation number.
// It starts with resident predecessor/config views and raw candidate bytes,
// then performs the production core's single decode, collated-data parse,
// state-update application, structural checks and semantic replay. Disk and
// master-view acquisition remain setup; no consensus engine is involved.
func BenchmarkValidateDiskCorpus(b *testing.B) {
	requireDiskCorpusSingleCPU(b)
	corpus := loadDiskCorpus(b)

	for _, variant := range []struct {
		name      string
		candidate *Candidate
	}{
		{name: "collated", candidate: corpus.fullCandidate},
		{name: "preloaded", candidate: corpus.preloadedCandidate},
	} {
		b.Run(variant.name, func(b *testing.B) {
			ctx := context.Background()
			semantics := NewSemanticVerifier(tvm.NewTVM())
			if err := validateDiskCorpusCandidate(ctx, corpus.request, variant.candidate, semantics); err != nil {
				b.Fatalf("validation preflight: %v", err)
			}

			b.ReportAllocs()
			b.ResetTimer()
			for b.Loop() {
				if err := validateDiskCorpusCandidate(ctx, corpus.request, variant.candidate, semantics); err != nil {
					b.Fatal(err)
				}
			}
			b.StopTimer()
			b.ReportMetric(float64(corpus.expectedTransactions), "tx/block")
			b.ReportMetric(float64(corpus.expectedGas), "gas/block")
			b.ReportMetric(float64(len(variant.candidate.BlockBOC)), "blockB")
			b.ReportMetric(float64(len(variant.candidate.CollatedData)), "collatedB")
		})
	}
}

// BenchmarkVerifyPreparedDiskCorpus is attribution-only. It times the exported
// verifier after Candidate.State and Candidate.StateUpdate were prepared; do
// not compare it to C++ run_validate_query.
func BenchmarkVerifyPreparedDiskCorpus(b *testing.B) {
	requireDiskCorpusSingleCPU(b)
	corpus := loadDiskCorpus(b)

	for _, variant := range []struct {
		name      string
		candidate *Candidate
	}{
		{name: "collated", candidate: corpus.fullCandidate},
		{name: "preloaded", candidate: corpus.preloadedCandidate},
	} {
		b.Run(variant.name, func(b *testing.B) {
			verification := diskCorpusVerification(corpus.request, variant.candidate)
			ctx := context.Background()
			if err := VerifyShardCandidate(ctx, verification); err != nil {
				b.Fatalf("validation preflight: %v", err)
			}

			b.ReportAllocs()
			b.ResetTimer()
			for b.Loop() {
				if err := VerifyShardCandidate(ctx, verification); err != nil {
					b.Fatal(err)
				}
			}
			b.StopTimer()
			b.ReportMetric(float64(corpus.expectedTransactions), "tx/block")
			b.ReportMetric(float64(corpus.expectedGas), "gas/block")
			b.ReportMetric(float64(len(variant.candidate.BlockBOC)), "blockB")
			b.ReportMetric(float64(len(variant.candidate.CollatedData)), "collatedB")
		})
	}
}

func loadDiskCorpus(tb testing.TB) *diskCorpus {
	tb.Helper()

	dir := os.Getenv(diskCorpusEnvironment)
	if dir == "" {
		tb.Skipf("set %s to a C++ collation fixture directory", diskCorpusEnvironment)
	}
	corpus, err := readDiskCorpus(dir)
	if err != nil {
		tb.Fatalf("load disk corpus %s: %v", dir, err)
	}

	return corpus
}

func readDiskCorpus(dir string) (*diskCorpus, error) {
	manifest, err := readDiskCorpusManifest(dir)
	if err != nil {
		return nil, err
	}
	values, err := validateDiskCorpusManifest(&manifest)
	if err != nil {
		return nil, err
	}
	shardStateMaxCells, err := diskCorpusShardStateMaxCells(manifest.Workload.Accounts)
	if err != nil {
		return nil, err
	}

	expectedPaths := diskCorpusPaths(manifest.Externals.Count, manifest.Previous.SeqNo)
	if len(manifest.Files) != len(expectedPaths) {
		return nil, fmt.Errorf("files has %d entries, want %d", len(manifest.Files), len(expectedPaths))
	}
	artifacts := make(map[string]diskCorpusArtifact, len(expectedPaths))
	artifactKeys := make([]string, 0, len(expectedPaths))
	for key := range expectedPaths {
		artifactKeys = append(artifactKeys, key)
	}
	sort.Strings(artifactKeys)
	for _, key := range artifactKeys {
		expectedPath := expectedPaths[key]
		entry, ok := manifest.Files[key]
		if !ok {
			return nil, fmt.Errorf("files is missing %q", key)
		}
		if entry.Path != expectedPath {
			return nil, fmt.Errorf("files.%s.path = %q, want %q", key, entry.Path, expectedPath)
		}
		maxCells := 0
		if key == "shard_prev_state" {
			maxCells = shardStateMaxCells
		}
		artifact, readErr := readDiskCorpusArtifact(dir, key, entry, maxCells)
		if readErr != nil {
			return nil, readErr
		}
		artifacts[key] = artifact
	}
	for key := range manifest.Files {
		if _, ok := expectedPaths[key]; !ok {
			return nil, fmt.Errorf("files contains unexpected entry %q", key)
		}
	}

	previousID, err := diskCorpusBlockID(manifest.Previous)
	if err != nil {
		return nil, fmt.Errorf("previous block id: %w", err)
	}
	masterID, err := diskCorpusBlockID(manifest.Masterchain)
	if err != nil {
		return nil, fmt.Errorf("masterchain block id: %w", err)
	}
	candidateID, err := diskCorpusBlockID(manifest.Candidate.Block)
	if err != nil {
		return nil, fmt.Errorf("candidate block id: %w", err)
	}

	previousBlock := diskCorpusSingleRoot(artifacts, "shard_prev_block")
	previous := PreviousBlock{
		ID:           previousID,
		Block:        previousBlock,
		State:        diskCorpusSingleRoot(artifacts, "shard_prev_state"),
		OutQueueSize: &values.outQueueSize,
	}
	if err = validateDiskCorpusStateIdentity("shard predecessor", previous.ID, previous.State); err != nil {
		return nil, err
	}
	previousState, err := verifyPredecessor("disk corpus shard", &previous)
	if err != nil {
		return nil, err
	}
	queueSize, err := exactOutQueueSize(previousState.OutMsgQueueInfo)
	if err != nil {
		return nil, err
	}
	if queueSize != values.outQueueSize {
		return nil, fmt.Errorf("predecessor outbound queue size = %d, manifest says %d", queueSize, values.outQueueSize)
	}

	if previous.ID.SeqNo == 0 {
		if err = diskCorpusCheckFileHash("shard zerostate", previous.ID.FileHash,
			artifacts["shard_prev_state"].raw); err != nil {
			return nil, err
		}
	} else if err = diskCorpusCheckFileHash("shard predecessor", previous.ID.FileHash,
		artifacts["shard_prev_block"].raw); err != nil {
		return nil, err
	}

	masterRoot := diskCorpusSingleRoot(artifacts, "masterchain_block")
	masterStateRoot := diskCorpusSingleRoot(artifacts, "masterchain_state")
	if err = validateDiskCorpusStateIdentity("masterchain", masterID, masterStateRoot); err != nil {
		return nil, err
	}
	if err = diskCorpusCheckFileHash("masterchain block", masterID.FileHash,
		artifacts["masterchain_block"].raw); err != nil {
		return nil, err
	}
	var masterBlock tlb.Block
	if err = parseExact(&masterBlock, masterRoot); err != nil {
		return nil, fmt.Errorf("decode disk corpus masterchain block: %w", err)
	}
	if masterBlock.GlobalID != diskCorpusGlobalID {
		return nil, fmt.Errorf("masterchain block global id = %d, want %d", masterBlock.GlobalID, diskCorpusGlobalID)
	}
	masterQueueSize, masterState, err := residentMasterchainPredecessor(masterID, masterRoot, masterStateRoot)
	if err != nil {
		return nil, fmt.Errorf("prepare disk corpus masterchain predecessor: %w", err)
	}
	masterPrevious := PreviousBlock{
		ID:           cloneBlockID(masterID),
		Block:        masterRoot,
		State:        masterStateRoot,
		OutQueueSize: &masterQueueSize,
	}
	snapshot, err := diskCorpusGroupSnapshot(masterID, masterStateRoot, masterState.GenUTime)
	if err != nil {
		return nil, err
	}
	acquisition := LocalAcquisition{
		configs: localConfigCache{entries: make(map[cell.Hash]localPreparedConfig)},
	}
	masterView, err := acquisition.masterView(masterPrevious, masterState, snapshot)
	if err != nil {
		return nil, fmt.Errorf("prepare masterchain context: %w", err)
	}
	globalID, err := (tlb.BlockchainConfig{Root: masterView.context.Config.execution.Root()}).GetGlobalID()
	if err != nil {
		return nil, fmt.Errorf("load corpus blockchain global id: %w", err)
	}
	if previousState.GlobalID != diskCorpusGlobalID || masterState.GlobalID != diskCorpusGlobalID ||
		globalID.GlobalID != diskCorpusGlobalID {
		return nil, fmt.Errorf(
			"fixture global ids shard/master/config = %d/%d/%d, want %d",
			previousState.GlobalID,
			masterState.GlobalID,
			globalID.GlobalID,
			diskCorpusGlobalID,
		)
	}
	if masterView.context.Config.execution.Root().HashKey() != snapshot.ConfigRootHash {
		return nil, errors.New("masterchain group snapshot configuration differs from state context")
	}
	if masterView.context.Config.capabilities&capFullCollatedData == 0 {
		return nil, errors.New("v1 masterchain config does not enable full collated data")
	}
	target := groups.ShardID{
		Workchain: manifest.Contract.Workchain,
		Shard:     int64(values.shard),
	}
	expectedNeighbors, err := expectedShardNeighbors(masterView.context, target)
	if err != nil {
		return nil, fmt.Errorf("derive disk corpus implicit neighbors: %w", err)
	}
	if err = validateDiskCorpusImplicitNeighbors(expectedNeighbors, previous.ID, masterID); err != nil {
		return nil, err
	}
	neighborViews := make(map[msgpool.ShardIdent]*localNeighborView, len(expectedNeighbors))
	setupContext := context.Background()
	neighbors, err := acquisition.loadExpectedNeighbors(
		setupContext,
		masterView,
		expectedNeighbors,
		[]PreviousBlock{previous},
		neighborViews,
		nil,
		true,
		acquisitionReadImmediate,
	)
	if err != nil {
		return nil, fmt.Errorf("load disk corpus implicit neighbors: %w", err)
	}
	if len(neighbors) != len(expectedNeighbors) {
		return nil, fmt.Errorf(
			"loaded %d disk corpus implicit neighbors, want %d",
			len(neighbors),
			len(expectedNeighbors),
		)
	}
	neighborShardEndLT, err := acquisition.historicalShardEndLT(
		setupContext,
		masterView,
		[]PreviousBlock{previous},
		neighbors,
		nil,
		nil,
		acquisitionReadImmediate,
	)
	if err != nil {
		return nil, fmt.Errorf("derive disk corpus neighbor end-lt view: %w", err)
	}
	setupUtime := max(previousState.GenUTime, masterState.GenUTime)
	if setupUtime == ^uint32(0) || manifest.Header.GenUtime != setupUtime+1 {
		return nil, errors.New("candidate time is not the next deterministic fixture time")
	}

	externals := make([]ExternalInput, manifest.Externals.Count)
	for i := range externals {
		key := fmt.Sprintf("external_%04d", i)
		root := diskCorpusSingleRoot(artifacts, key)
		prepared, prepareErr := tvm.PrepareMessage(root)
		if prepareErr != nil {
			return nil, fmt.Errorf("prepare %s: %w", key, prepareErr)
		}
		externals[i] = ExternalInput{
			Ref: msgpool.ExternalRef{
				Hash:       root.HashKey(),
				Generation: uint64(i) + 1,
			},
			message: prepared,
		}
	}

	request := ShardRequest{
		Shard:       target,
		Previous:    previous,
		Masterchain: masterView.context,
		Header: HeaderParams{
			GenUtime:   manifest.Header.GenUtime,
			GenUtimeMS: values.genUtimeMS,
		},
		BeforeSplit:         false,
		RandSeed:            values.randSeed,
		CreatedBy:           values.createdBy,
		Externals:           externals,
		MaxExternalAttempts: int(manifest.Contract.MaxExternalAttempts),
		Dispatch:            ReferenceDispatchPolicy(),
		Internals:           &msgpool.Cut{},
		Neighbors:           neighbors,
		FullCollatedProofs:  &localFullProofProvider{proofViews: neighborViews, messageViews: neighborViews},
		NeighborShardEndLT:  neighborShardEndLT,
	}
	if err = validateDiskCorpusCreator(snapshot, request.Shard, values.createdBy); err != nil {
		return nil, err
	}

	blockArtifact := artifacts["candidate_block"]
	if err = diskCorpusCheckFileHash("candidate block", candidateID.FileHash, blockArtifact.raw); err != nil {
		return nil, err
	}
	candidateRoot := diskCorpusSingleRoot(artifacts, "candidate_block")
	if !bytes.Equal(candidateRoot.Hash(), candidateID.RootHash) {
		return nil, fmt.Errorf("candidate root hash = %x, want %x", candidateRoot.Hash(), candidateID.RootHash)
	}
	var candidateBlock tlb.Block
	if err = parseExact(&candidateBlock, candidateRoot); err != nil {
		return nil, fmt.Errorf("decode candidate block: %w", err)
	}
	if err = verifyExactBlockParts(candidateRoot, &candidateBlock); err != nil {
		return nil, err
	}
	if err = verifyHeaderAndID(&candidateBlock.BlockInfo, &Candidate{ID: candidateID}); err != nil {
		return nil, err
	}
	if err = validateDiskCorpusCandidateHeader(&manifest, values, &candidateBlock); err != nil {
		return nil, err
	}
	successor, err := cell.ApplyMerkleUpdate(previous.State, candidateBlock.StateUpdate)
	if err != nil {
		return nil, fmt.Errorf("apply candidate state update: %w", err)
	}
	if successor.HashKey() != values.stateRootHash {
		return nil, fmt.Errorf("candidate successor state hash = %x, want %x",
			successor.HashKey(), values.stateRootHash)
	}
	if err = validateDiskCorpusStateIdentity("candidate successor", candidateID, successor); err != nil {
		return nil, err
	}

	full := artifacts["candidate_collated_full"].raw
	preloaded := artifacts["candidate_collated_preloaded"].raw
	fullHash := sha256.Sum256(full)
	if fullHash != values.collatedHash {
		return nil, fmt.Errorf("full collated file hash = %x, want %x", fullHash, values.collatedHash)
	}
	preloadedHash := sha256.Sum256(preloaded)
	if preloadedHash != values.preloadedCollatedHash {
		return nil, fmt.Errorf("preloaded collated file hash = %x, want %x",
			preloadedHash, values.preloadedCollatedHash)
	}

	fullCandidate := diskCorpusCandidateFrom(
		candidateID,
		values.createdBy,
		blockArtifact.raw,
		full,
		fullHash,
		successor,
		candidateBlock.StateUpdate,
	)
	preloadedCandidate := diskCorpusCandidateFrom(
		candidateID,
		values.createdBy,
		blockArtifact.raw,
		preloaded,
		preloadedHash,
		successor,
		candidateBlock.StateUpdate,
	)
	fullCollated, err := verifyCollatedData(fullCandidate, nil, manifest.Header.GenUtime)
	if err != nil {
		return nil, fmt.Errorf("verify full collated data: %w", err)
	}
	if !fullCollated.full || fullCollated.genUtimeMS != values.genUtimeMS {
		return nil, errors.New("full collated data does not satisfy the v1 proof/time contract")
	}
	preloadedCollated, err := verifyCollatedData(preloadedCandidate, nil, manifest.Header.GenUtime)
	if err != nil {
		return nil, fmt.Errorf("verify preloaded collated data: %w", err)
	}
	if preloadedCollated.full || preloadedCollated.genUtimeMS != values.genUtimeMS {
		return nil, errors.New("preloaded collated data does not satisfy the resident-state/time contract")
	}

	return &diskCorpus{
		request:              request,
		fullCandidate:        fullCandidate,
		preloadedCandidate:   preloadedCandidate,
		expectedTransactions: manifest.Expected.Transactions,
		expectedGas:          values.expectedGas,
	}, nil
}

func validateDiskCorpusCreator(snapshot *groups.Snapshot, shard groups.ShardID, createdBy [32]byte) error {
	var matched *groups.Session
	for i := range snapshot.Active {
		if snapshot.Active[i].Shard != shard {
			continue
		}
		if matched != nil {
			return errors.New("masterchain state yielded duplicate target validator groups")
		}
		matched = &snapshot.Active[i]
	}
	if matched == nil || len(matched.Validators) == 0 {
		return errors.New("masterchain state yielded no target validator group")
	}
	if matched.Validators[0].PublicKey != createdBy {
		return errors.New("candidate creator is not the deterministic first target validator")
	}

	return nil
}

func validateDiskCorpusStateIdentity(label string, id ton.BlockIDExt, root *cell.Cell) error {
	var state tlb.ShardStateUnsplit
	if err := parseExact(&state, root); err != nil {
		return fmt.Errorf("decode disk corpus %s state: %w", label, err)
	}
	expected, err := topologyShardIdent(groups.ShardID{
		Workchain: id.Workchain,
		Shard:     id.Shard,
	})
	if err != nil {
		return fmt.Errorf("decode disk corpus %s block id: %w", label, err)
	}
	if state.ShardIdent != expected || state.Seqno != id.SeqNo {
		return fmt.Errorf(
			"%s manifest/state identity differs: manifest=%d:%016x/%d "+
				"expected-ident={bits:%d prefix:%016x} state=%d:%016x/%d ident={bits:%d prefix:%016x}",
			label,
			id.Workchain,
			uint64(id.Shard),
			id.SeqNo,
			expected.PrefixBits,
			expected.ShardPrefix,
			state.ShardIdent.WorkchainID,
			state.ShardIdent.GetShardID(),
			state.Seqno,
			state.ShardIdent.PrefixBits,
			state.ShardIdent.ShardPrefix,
		)
	}

	return nil
}

type diskCorpusValues struct {
	shard                 uint64
	outQueueSize          uint64
	genUtimeMS            uint64
	randSeed              [32]byte
	createdBy             [32]byte
	collatedHash          [32]byte
	preloadedCollatedHash [32]byte
	stateRootHash         [32]byte
	expectedGas           uint64
}

// Transactions one transfer of the named v1 workload must produce: four for the
// jetton chain (source wallet, both jetton wallets, recipient notification),
// two for a plain wallet-to-wallet transfer (source wallet, recipient credit).
func diskCorpusTransactionsPerTransfer(name string) (uint32, bool) {
	switch name {
	case diskCorpusWorkloadNameJetton:
		return 4, true
	case diskCorpusWorkloadNameTransfer:
		return 2, true
	default:
		return 0, false
	}
}

func validateDiskCorpusManifest(manifest *diskCorpusManifest) (diskCorpusValues, error) {
	if manifest.Format != diskCorpusFormat || manifest.Version != diskCorpusVersion {
		return diskCorpusValues{}, fmt.Errorf("fixture format/version = %q/%d, want %q/%d",
			manifest.Format, manifest.Version, diskCorpusFormat, diskCorpusVersion)
	}
	transactionsPerTransfer, workloadKnown := diskCorpusTransactionsPerTransfer(manifest.Workload.Name)
	if !workloadKnown || manifest.Workload.Version != 1 ||
		manifest.Workload.Accounts == 0 || manifest.Workload.Transfers == 0 ||
		manifest.Workload.Transfers > 9999 {
		return diskCorpusValues{}, errors.New("workload name/version/accounts/transfers are invalid")
	}
	if _, err := diskCorpusCanonicalUint("workload.seed", manifest.Workload.Seed, 64); err != nil {
		return diskCorpusValues{}, err
	}
	contract := manifest.Contract
	if contract.Workchain != 0 || contract.PreviousCount != 1 || contract.BeforeSplit ||
		contract.AfterSplit || contract.AfterMerge || contract.InternalsCount != 0 ||
		!contract.InternalsComplete || contract.NeighborsCount != 0 || contract.TopBlocksCount != 0 ||
		!contract.FullCollatedData || contract.StorageStatCache || contract.QueueCleanupDeadline ||
		contract.InternalMsgDeadline {
		return diskCorpusValues{}, errors.New("fixture does not satisfy the external-only v1 contract")
	}
	if contract.MaxExternalAttempts != manifest.Externals.Count {
		return diskCorpusValues{}, errors.New("max_external_attempts differs from external count")
	}
	if manifest.Externals.Count == 0 || manifest.Externals.Count != manifest.Workload.Transfers ||
		manifest.Externals.Pattern != "externals/%04d.boc" {
		return diskCorpusValues{}, errors.New("external count/pattern differs from the v1 contract")
	}

	shard, err := diskCorpusShard("contract.shard", contract.Shard)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if shard != msgpool.ShardAll {
		return diskCorpusValues{}, errors.New("contract shard is not the full wc0 shard")
	}
	outQueueSize, err := diskCorpusCanonicalUint("contract.out_queue_size", contract.OutQueueSize, 64)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if outQueueSize != 0 {
		return diskCorpusValues{}, errors.New("v1 predecessor outbound queue is not empty")
	}
	genUtimeMS, err := diskCorpusCanonicalUint("header.gen_utime_ms", manifest.Header.GenUtimeMS, 64)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if genUtimeMS/1_000 != uint64(manifest.Header.GenUtime) {
		return diskCorpusValues{}, errors.New("header generation milliseconds disagree with generation seconds")
	}
	if manifest.Header.GenUtime == 0 {
		return diskCorpusValues{}, errors.New("header generation time is zero")
	}
	randSeed, err := diskCorpusHash("header.rand_seed", manifest.Header.RandSeed)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if randSeed == ([32]byte{}) {
		return diskCorpusValues{}, errors.New("header random seed is zero")
	}
	createdBy, err := diskCorpusHash("header.created_by", manifest.Header.CreatedBy)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if createdBy == ([32]byte{}) {
		return diskCorpusValues{}, errors.New("header creator is zero")
	}
	collatedHash, err := diskCorpusHash("candidate.collated_file_hash", manifest.Candidate.CollatedFileHash)
	if err != nil {
		return diskCorpusValues{}, err
	}
	preloadedCollatedHash, err := diskCorpusHash(
		"candidate.preloaded_collated_file_hash",
		manifest.Candidate.PreloadedCollatedFileHash,
	)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if collatedHash == preloadedCollatedHash {
		return diskCorpusValues{}, errors.New("full and preloaded collated file hashes are equal")
	}
	stateRootHash, err := diskCorpusHash("candidate.state_root_hash", manifest.Candidate.StateRootHash)
	if err != nil {
		return diskCorpusValues{}, err
	}
	expectedGas, err := diskCorpusCanonicalUint("expected.gas_used", manifest.Expected.GasUsed, 64)
	if err != nil {
		return diskCorpusValues{}, err
	}
	if manifest.Expected.Transactions == 0 {
		return diskCorpusValues{}, errors.New("expected transaction count is zero")
	}
	if manifest.Expected.Transactions != transactionsPerTransfer*manifest.Workload.Transfers {
		return diskCorpusValues{}, errors.New("expected transaction count differs from the v1 workload")
	}
	if err = validateDiskCorpusIdentities(manifest, shard); err != nil {
		return diskCorpusValues{}, err
	}
	if err = validateDiskCorpusExpectedSizes(manifest); err != nil {
		return diskCorpusValues{}, err
	}

	return diskCorpusValues{
		shard:                 shard,
		outQueueSize:          outQueueSize,
		genUtimeMS:            genUtimeMS,
		randSeed:              randSeed,
		createdBy:             createdBy,
		collatedHash:          collatedHash,
		preloadedCollatedHash: preloadedCollatedHash,
		stateRootHash:         stateRootHash,
		expectedGas:           expectedGas,
	}, nil
}

func validateDiskCorpusIdentities(manifest *diskCorpusManifest, shard uint64) error {
	for label, block := range map[string]diskCorpusBlock{
		"previous":  manifest.Previous,
		"candidate": manifest.Candidate.Block,
	} {
		parsed, err := diskCorpusShard(label+".shard", block.Shard)
		if err != nil {
			return err
		}
		if block.Workchain != manifest.Contract.Workchain || parsed != shard {
			return fmt.Errorf("%s block is outside contract shard", label)
		}
	}
	masterShard, err := diskCorpusShard("masterchain.shard", manifest.Masterchain.Shard)
	if err != nil {
		return err
	}
	if manifest.Masterchain.Workchain != masterchainWorkchainID || int64(masterShard) != int64(-1<<63) {
		return errors.New("masterchain block is not the masterchain root shard")
	}
	if manifest.Candidate.Block.SeqNo != manifest.Previous.SeqNo+1 {
		return errors.New("candidate is not the immediate predecessor successor")
	}
	if manifest.Previous.SeqNo != 0 || manifest.Masterchain.SeqNo == 0 {
		return errors.New("previous/masterchain sequence numbers violate the v1 profile")
	}

	return nil
}

func validateDiskCorpusExpectedSizes(manifest *diskCorpusManifest) error {
	checks := []struct {
		label string
		value string
		key   string
	}{
		{label: "expected.block_bytes", value: manifest.Expected.BlockBytes, key: "candidate_block"},
		{label: "expected.collated_full_bytes", value: manifest.Expected.CollatedFullBytes, key: "candidate_collated_full"},
		{label: "expected.collated_preloaded_bytes", value: manifest.Expected.CollatedPreloadedBytes, key: "candidate_collated_preloaded"},
	}
	for _, check := range checks {
		expected, err := diskCorpusCanonicalUint(check.label, check.value, 64)
		if err != nil {
			return err
		}
		entry, ok := manifest.Files[check.key]
		if !ok {
			return fmt.Errorf("files is missing %q", check.key)
		}
		actual, err := diskCorpusCanonicalUint("files."+check.key+".size", entry.Size, 64)
		if err != nil {
			return err
		}
		if expected != actual {
			return fmt.Errorf("%s = %d, artifact size is %d", check.label, expected, actual)
		}
	}

	return nil
}

func readDiskCorpusManifest(dir string) (diskCorpusManifest, error) {
	meta, err := os.ReadFile(filepath.Join(dir, diskCorpusMetaName))
	if err != nil {
		return diskCorpusManifest{}, fmt.Errorf("read %s: %w", diskCorpusMetaName, err)
	}
	sum := sha256.Sum256(meta)
	wantSum := fmt.Sprintf("%x  %s\n", sum, diskCorpusMetaName)
	manifestSum, err := os.ReadFile(filepath.Join(dir, diskCorpusSumName))
	if err != nil {
		return diskCorpusManifest{}, fmt.Errorf("read %s: %w", diskCorpusSumName, err)
	}
	if string(manifestSum) != wantSum {
		return diskCorpusManifest{}, fmt.Errorf("%s does not match exact %s bytes", diskCorpusSumName, diskCorpusMetaName)
	}
	if err = rejectDiskCorpusDuplicateKeys(meta); err != nil {
		return diskCorpusManifest{}, fmt.Errorf("decode %s: %w", diskCorpusMetaName, err)
	}
	if err = validateDiskCorpusJSONKeys(meta); err != nil {
		return diskCorpusManifest{}, fmt.Errorf("decode %s: %w", diskCorpusMetaName, err)
	}

	decoder := json.NewDecoder(bytes.NewReader(meta))
	decoder.DisallowUnknownFields()
	var manifest diskCorpusManifest
	if err = decoder.Decode(&manifest); err != nil {
		return diskCorpusManifest{}, fmt.Errorf("decode %s: %w", diskCorpusMetaName, err)
	}
	if err = diskCorpusJSONEnd(decoder); err != nil {
		return diskCorpusManifest{}, fmt.Errorf("decode %s: %w", diskCorpusMetaName, err)
	}

	return manifest, nil
}

func diskCorpusPaths(externals uint32, previousSeqno uint32) map[string]string {
	paths := map[string]string{
		"shard_prev_state":             "states/shard-prev.boc",
		"masterchain_state":            "states/masterchain.boc",
		"masterchain_block":            "blocks/masterchain.boc",
		"candidate_block":              "candidate/block.boc",
		"candidate_collated_full":      "candidate/collated-full.boc",
		"candidate_collated_preloaded": "candidate/collated-preloaded.boc",
	}
	if previousSeqno != 0 {
		paths["shard_prev_block"] = "blocks/shard-prev.boc"
	}
	for i := range externals {
		paths[fmt.Sprintf("external_%04d", i)] = fmt.Sprintf("externals/%04d.boc", i)
	}

	return paths
}

func readDiskCorpusArtifact(dir, key string, entry diskCorpusFile, maxCells int) (diskCorpusArtifact, error) {
	size, err := diskCorpusCanonicalUint("files."+key+".size", entry.Size, 64)
	if err != nil {
		return diskCorpusArtifact{}, err
	}
	wantSHA, err := diskCorpusHash("files."+key+".sha256", entry.SHA256)
	if err != nil {
		return diskCorpusArtifact{}, err
	}
	if entry.RootCount == 0 || uint32(len(entry.RootHashes)) != entry.RootCount {
		return diskCorpusArtifact{}, fmt.Errorf("files.%s root count/list is invalid", key)
	}
	raw, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(entry.Path)))
	if err != nil {
		return diskCorpusArtifact{}, fmt.Errorf("read files.%s: %w", key, err)
	}
	if uint64(len(raw)) != size {
		return diskCorpusArtifact{}, fmt.Errorf("files.%s size = %d, want %d", key, len(raw), size)
	}
	gotSHA := sha256.Sum256(raw)
	if gotSHA != wantSHA {
		return diskCorpusArtifact{}, fmt.Errorf("files.%s sha256 = %x, want %x", key, gotSHA, wantSHA)
	}
	roots, err := cell.FromBOCMultiRootWithOptions(raw, cell.BOCParseOptions{MaxCells: maxCells})
	if err != nil {
		if maxCells > 0 {
			return diskCorpusArtifact{}, fmt.Errorf(
				"parse files.%s boc with v1 cell ceiling %d (%d*workload.accounts+%d): %w",
				key,
				maxCells,
				diskCorpusShardStateCellsPerAccount,
				diskCorpusShardStateFixedCells,
				err,
			)
		}
		return diskCorpusArtifact{}, fmt.Errorf("parse files.%s boc: %w", key, err)
	}
	if uint32(len(roots)) != entry.RootCount {
		return diskCorpusArtifact{}, fmt.Errorf("files.%s BOC root count = %d, want %d", key, len(roots), entry.RootCount)
	}
	for i := range roots {
		want, hashErr := diskCorpusHash(fmt.Sprintf("files.%s.root_hashes[%d]", key, i), entry.RootHashes[i])
		if hashErr != nil {
			return diskCorpusArtifact{}, hashErr
		}
		if got := roots[i].HashKey(); got != want {
			return diskCorpusArtifact{}, fmt.Errorf("files.%s root %d hash = %x, want %x", key, i, got, want)
		}
	}
	if key != "candidate_collated_full" && key != "candidate_collated_preloaded" && len(roots) != 1 {
		return diskCorpusArtifact{}, fmt.Errorf("files.%s must contain exactly one BOC root", key)
	}

	return diskCorpusArtifact{raw: raw, roots: roots}, nil
}

func diskCorpusShardStateMaxCells(accounts uint32) (int, error) {
	accountCount := uint64(accounts)
	if accountCount > (^uint64(0)-diskCorpusShardStateFixedCells)/diskCorpusShardStateCellsPerAccount {
		return 0, errors.New("v1 shard-state cell ceiling overflows uint64")
	}
	maxCells := accountCount*diskCorpusShardStateCellsPerAccount + diskCorpusShardStateFixedCells
	maxInt := uint64(^uint(0) >> 1)
	if maxCells > maxInt {
		return 0, fmt.Errorf("v1 shard-state cell ceiling %d does not fit int", maxCells)
	}

	return int(maxCells), nil
}

func diskCorpusSingleRoot(artifacts map[string]diskCorpusArtifact, key string) *cell.Cell {
	artifact, ok := artifacts[key]
	if !ok {
		return nil
	}
	return artifact.roots[0]
}

func diskCorpusBlockID(block diskCorpusBlock) (ton.BlockIDExt, error) {
	shard, err := diskCorpusShard("block.shard", block.Shard)
	if err != nil {
		return ton.BlockIDExt{}, err
	}
	rootHash, err := diskCorpusHash("block.root_hash", block.RootHash)
	if err != nil {
		return ton.BlockIDExt{}, err
	}
	fileHash, err := diskCorpusHash("block.file_hash", block.FileHash)
	if err != nil {
		return ton.BlockIDExt{}, err
	}

	return ton.BlockIDExt{
		Workchain: block.Workchain,
		Shard:     int64(shard),
		SeqNo:     block.SeqNo,
		RootHash:  append([]byte(nil), rootHash[:]...),
		FileHash:  append([]byte(nil), fileHash[:]...),
	}, nil
}

func diskCorpusGroupSnapshot(
	block ton.BlockIDExt,
	stateRoot *cell.Cell,
	genUtime uint32,
) (*groups.Snapshot, error) {
	state, err := groups.ParseState(groups.StateInput{Block: block, Root: stateRoot})
	if err != nil {
		return nil, fmt.Errorf("parse disk corpus validator-group state: %w", err)
	}
	if !state.IsKeyState || !state.RotatedAllShards {
		return nil, errors.New("v1 masterchain state is not a self-contained key/all-shards rotation")
	}
	tracker, err := groups.NewTracker(groups.TrackerOptions{})
	if err != nil {
		return nil, err
	}
	tracked, err := tracker.Apply(groups.ApplyInput{
		Block: block,
		Root:  stateRoot,
		AsOf:  time.Unix(int64(genUtime), 0),
	})
	if err != nil {
		return nil, fmt.Errorf("derive validator groups from masterchain state: %w", err)
	}
	if !tracked.Snapshot.Ready {
		return nil, errors.New("masterchain state did not yield a ready validator-group snapshot")
	}

	return tracked.Snapshot, nil
}

func validateDiskCorpusCandidateHeader(
	manifest *diskCorpusManifest,
	values diskCorpusValues,
	block *tlb.Block,
) error {
	header := &block.BlockInfo
	if block.GlobalID != diskCorpusGlobalID || header.AfterSplit || header.AfterMerge || header.BeforeSplit ||
		header.SeqNo != manifest.Candidate.Block.SeqNo ||
		header.GenUtime != manifest.Header.GenUtime {
		return errors.New("candidate header differs from the v1 contract")
	}
	if block.Extra == nil || !bytes.Equal(block.Extra.RandSeed, values.randSeed[:]) ||
		!bytes.Equal(block.Extra.CreatedBy, values.createdBy[:]) {
		return errors.New("candidate random seed or creator differs from manifest")
	}

	return nil
}

func validateDiskCorpusImplicitNeighbors(
	expected map[neighborShardKey]ton.BlockIDExt,
	previous ton.BlockIDExt,
	master ton.BlockIDExt,
) error {
	if len(expected) != 2 {
		return fmt.Errorf("v1 topology derived %d implicit neighbors, want exactly 2", len(expected))
	}

	required := []struct {
		label string
		id    ton.BlockIDExt
	}{
		{label: "own predecessor", id: previous},
		{label: "referenced masterchain block", id: master},
	}
	seen := make(map[neighborShardKey]struct{}, len(required))
	for _, item := range required {
		key := neighborShardKey{workchain: item.id.Workchain, shard: item.id.Shard}
		if _, duplicate := seen[key]; duplicate {
			return fmt.Errorf("v1 implicit neighbor identities collide at %d:%016x", key.workchain, uint64(key.shard))
		}
		seen[key] = struct{}{}

		actual, ok := expected[key]
		if !ok {
			return fmt.Errorf(
				"v1 topology omitted implicit %s %d:%016x:%d",
				item.label,
				item.id.Workchain,
				uint64(item.id.Shard),
				item.id.SeqNo,
			)
		}
		if !actual.Equals(&item.id) {
			return fmt.Errorf(
				"v1 implicit %s block = %d:%016x:%d, want %d:%016x:%d",
				item.label,
				actual.Workchain,
				uint64(actual.Shard),
				actual.SeqNo,
				item.id.Workchain,
				uint64(item.id.Shard),
				item.id.SeqNo,
			)
		}
	}

	return nil
}

func diskCorpusCandidateFrom(
	id ton.BlockIDExt,
	createdBy [32]byte,
	blockBOC []byte,
	collated []byte,
	collatedHash [32]byte,
	state *cell.Cell,
	stateUpdate *cell.Cell,
) *Candidate {
	return &Candidate{
		ID:               cloneBlockID(id),
		CreatedBy:        createdBy,
		BlockBOC:         blockBOC,
		CollatedData:     collated,
		CollatedFileHash: collatedHash,
		State:            state,
		StateUpdate:      stateUpdate,
	}
}

func diskCorpusVerification(req ShardRequest, candidate *Candidate) ShardVerificationRequest {
	return ShardVerificationRequest{
		Previous:           req.Previous,
		Masterchain:        req.Masterchain,
		Neighbors:          req.Neighbors,
		NeighborShardEndLT: req.NeighborShardEndLT,
		Semantics:          NewSemanticVerifier(tvm.NewTVM()),
		Candidate:          candidate,
	}
}

func validateDiskCorpusCandidate(
	ctx context.Context,
	req ShardRequest,
	candidate *Candidate,
	semantics CandidateTransitionVerifier,
) error {
	prepared, err := prepareValidationCandidate(
		ctx,
		CandidateArtifact{
			Candidate: simplex.Candidate{
				Block:            cloneBlockID(candidate.ID),
				CollatedFileHash: candidate.CollatedFileHash,
			},
			BlockBOC:     candidate.BlockBOC,
			CollatedData: candidate.CollatedData,
		},
		candidate.CreatedBy,
		false,
		[]PreviousBlock{req.Previous},
	)
	if err != nil {
		return err
	}
	if err = prepared.verifyCreator(); err != nil {
		return err
	}
	if err = prepared.bindConfig(ctx, req.Masterchain.Config); err != nil {
		return err
	}
	verification := ShardVerificationRequest{
		Previous:           prepared.previous[0],
		Masterchain:        req.Masterchain,
		Neighbors:          req.Neighbors,
		NeighborShardEndLT: req.NeighborShardEndLT,
		Semantics:          semantics,
		Candidate:          prepared.candidate,
		stateProven:        prepared.verified.collated.full,
	}

	return verifyPreparedShardCandidate(ctx, verification, &prepared.verified)
}

func assertDiskCorpusCandidate(tb testing.TB, corpus *diskCorpus, candidate *Candidate) {
	tb.Helper()

	if candidate.State.HashKey() != corpus.fullCandidate.State.HashKey() {
		tb.Fatalf("successor state hash = %x, want %x",
			candidate.State.HashKey(), corpus.fullCandidate.State.HashKey())
	}
	stats := candidate.Stats
	if stats.Transactions != corpus.expectedTransactions || stats.GasUsed != corpus.expectedGas {
		tb.Fatalf("candidate transactions/gas = %d/%d, want %d/%d",
			stats.Transactions, stats.GasUsed, corpus.expectedTransactions, corpus.expectedGas)
	}
	if stats.ExternalAttempts != uint32(len(corpus.request.Externals)) ||
		stats.ExternalIncluded != uint32(len(corpus.request.Externals)) ||
		stats.ExternalInvalid != 0 || stats.ExternalNotAccepted != 0 || stats.ExternalSkippedLimit != 0 ||
		stats.InternalsImported != 0 || stats.InternalMsgTimeouts != 0 || stats.ProcessedScanTraceError != "" {
		tb.Fatalf("candidate does not satisfy the complete external-only contract: %+v", stats)
	}
}

func diskCorpusCheckFileHash(label string, expected []byte, raw []byte) error {
	actual := sha256.Sum256(raw)
	if !bytes.Equal(expected, actual[:]) {
		return fmt.Errorf("%s file hash = %x, want %x", label, actual, expected)
	}

	return nil
}

func diskCorpusHash(label, value string) ([32]byte, error) {
	if len(value) != 64 || value != strings.ToLower(value) {
		return [32]byte{}, fmt.Errorf("%s is not canonical lowercase 256-bit hex", label)
	}
	decoded, err := hex.DecodeString(value)
	if err != nil {
		return [32]byte{}, fmt.Errorf("%s: %w", label, err)
	}
	var result [32]byte
	copy(result[:], decoded)

	return result, nil
}

func diskCorpusShard(label, value string) (uint64, error) {
	if len(value) != 16 || value != strings.ToLower(value) {
		return 0, fmt.Errorf("%s is not canonical 64-bit hex", label)
	}
	shard, err := strconv.ParseUint(value, 16, 64)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", label, err)
	}

	return shard, nil
}

func diskCorpusCanonicalUint(label, value string, bits int) (uint64, error) {
	if value == "" || value != "0" && value[0] == '0' {
		return 0, fmt.Errorf("%s is not canonical decimal", label)
	}
	for _, digit := range value {
		if digit < '0' || digit > '9' {
			return 0, fmt.Errorf("%s is not canonical decimal", label)
		}
	}
	parsed, err := strconv.ParseUint(value, 10, bits)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", label, err)
	}

	return parsed, nil
}

func rejectDiskCorpusDuplicateKeys(raw []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if err := scanDiskCorpusJSONValue(decoder, "$"); err != nil {
		return err
	}

	return diskCorpusJSONEnd(decoder)
}

func scanDiskCorpusJSONValue(decoder *json.Decoder, path string) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delim, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delim {
	case '{':
		seen := make(map[string]struct{})
		for decoder.More() {
			keyToken, keyErr := decoder.Token()
			if keyErr != nil {
				return keyErr
			}
			key, ok := keyToken.(string)
			if !ok {
				return fmt.Errorf("%s object key is not a string", path)
			}
			if _, duplicate := seen[key]; duplicate {
				return fmt.Errorf("%s has duplicate key %q", path, key)
			}
			seen[key] = struct{}{}
			if err = scanDiskCorpusJSONValue(decoder, path+"."+key); err != nil {
				return err
			}
		}
		_, err = decoder.Token()
		return err
	case '[':
		for index := 0; decoder.More(); index++ {
			if err = scanDiskCorpusJSONValue(decoder, fmt.Sprintf("%s[%d]", path, index)); err != nil {
				return err
			}
		}
		_, err = decoder.Token()
		return err
	default:
		return fmt.Errorf("unexpected JSON delimiter %q at %s", delim, path)
	}
}

func validateDiskCorpusJSONKeys(raw []byte) error {
	var top map[string]json.RawMessage
	if err := json.Unmarshal(raw, &top); err != nil {
		return err
	}
	if err := requireDiskCorpusKeys("$", top,
		"format", "version", "workload", "contract", "header", "previous", "masterchain",
		"candidate", "externals", "files", "expected"); err != nil {
		return err
	}
	if err := requireDiskCorpusObjectKeys(top, "workload",
		"name", "version", "accounts", "transfers", "seed"); err != nil {
		return err
	}
	if err := requireDiskCorpusObjectKeys(top, "contract",
		"workchain", "shard", "previous_count", "before_split", "after_split", "after_merge",
		"internals_count", "internals_complete", "neighbors_count", "top_blocks_count", "out_queue_size",
		"full_collated_data", "max_external_attempts", "storage_stat_cache", "queue_cleanup_deadline",
		"internal_msg_deadline"); err != nil {
		return err
	}
	if err := requireDiskCorpusObjectKeys(top, "header",
		"gen_utime", "gen_utime_ms", "rand_seed", "created_by"); err != nil {
		return err
	}
	for _, key := range []string{"previous", "masterchain"} {
		if err := requireDiskCorpusObjectKeys(top, key,
			"workchain", "shard", "seqno", "root_hash", "file_hash"); err != nil {
			return err
		}
	}
	var candidate map[string]json.RawMessage
	if err := json.Unmarshal(top["candidate"], &candidate); err != nil {
		return fmt.Errorf("candidate: %w", err)
	}
	if err := requireDiskCorpusKeys("candidate", candidate,
		"block", "collated_file_hash", "preloaded_collated_file_hash", "state_root_hash"); err != nil {
		return err
	}
	var candidateBlock map[string]json.RawMessage
	if err := json.Unmarshal(candidate["block"], &candidateBlock); err != nil {
		return fmt.Errorf("candidate.block: %w", err)
	}
	if err := requireDiskCorpusKeys("candidate.block", candidateBlock,
		"workchain", "shard", "seqno", "root_hash", "file_hash"); err != nil {
		return err
	}
	if err := requireDiskCorpusObjectKeys(top, "externals", "count", "pattern"); err != nil {
		return err
	}
	if err := requireDiskCorpusObjectKeys(top, "expected",
		"transactions", "gas_used", "block_bytes", "collated_full_bytes", "collated_preloaded_bytes"); err != nil {
		return err
	}
	var files map[string]json.RawMessage
	if err := json.Unmarshal(top["files"], &files); err != nil {
		return fmt.Errorf("files: %w", err)
	}
	if len(files) == 0 {
		return errors.New("files is empty")
	}
	for key, rawFile := range files {
		var file map[string]json.RawMessage
		if err := json.Unmarshal(rawFile, &file); err != nil {
			return fmt.Errorf("files.%s: %w", key, err)
		}
		if err := requireDiskCorpusKeys("files."+key, file,
			"path", "size", "sha256", "root_count", "root_hashes"); err != nil {
			return err
		}
	}

	return nil
}

func requireDiskCorpusObjectKeys(
	top map[string]json.RawMessage,
	key string,
	expected ...string,
) error {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(top[key], &object); err != nil {
		return fmt.Errorf("%s: %w", key, err)
	}

	return requireDiskCorpusKeys(key, object, expected...)
}

func requireDiskCorpusKeys(path string, object map[string]json.RawMessage, expected ...string) error {
	if len(object) != len(expected) {
		return fmt.Errorf("%s has %d fields, want %d", path, len(object), len(expected))
	}
	for _, key := range expected {
		if _, ok := object[key]; !ok {
			return fmt.Errorf("%s is missing %q", path, key)
		}
	}

	return nil
}

func diskCorpusJSONEnd(decoder *json.Decoder) error {
	var trailing any
	if err := decoder.Decode(&trailing); errors.Is(err, io.EOF) {
		return nil
	} else if err != nil {
		return err
	}

	return errors.New("trailing JSON value")
}
