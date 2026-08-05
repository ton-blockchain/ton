// quicmsgbench-go implements the transport benchmark workload contract with quic-go.
package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"flag"
	"fmt"
	"io"
	"math"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/quic-go/quic-go"
)

const (
	alpn         = "ton-transport-bench"
	messageByte  = byte('M')
	queryByte    = byte('Q')
	responseByte = byte('R')
	probeByte    = byte('P')
)

type options struct {
	server          bool
	client          bool
	addr            string
	statsAddr       string
	serverAddr      string
	servers         int
	connections     int
	connectInflight int
	connectRate     float64
	workload        string
	size            int
	responseSize    int
	rate            int
	burst           int
	inflight        int
	threads         int
	tuned           bool
	duration        float64
}

type counters struct {
	connections      atomic.Int64
	messagesSent     atomic.Uint64
	messagesReceived atomic.Uint64
	queriesSent      atomic.Uint64
	queriesReceived  atomic.Uint64
	queriesCompleted atomic.Uint64
	errors           atomic.Uint64
	bytesSent        atomic.Uint64
	bytesReceived    atomic.Uint64
	firstPassNanos   atomic.Uint64
	setupNanoseconds atomic.Uint64
	setupReconnects  atomic.Uint64
}

func (c *counters) write(w io.Writer) {
	fmt.Fprintf(w, "bench_connections_current %d\n", c.connections.Load())
	fmt.Fprintf(w, "bench_messages_sent_total %d\n", c.messagesSent.Load())
	fmt.Fprintf(w, "bench_messages_received_total %d\n", c.messagesReceived.Load())
	fmt.Fprintf(w, "bench_queries_sent_total %d\n", c.queriesSent.Load())
	fmt.Fprintf(w, "bench_queries_received_total %d\n", c.queriesReceived.Load())
	fmt.Fprintf(w, "bench_queries_completed_total %d\n", c.queriesCompleted.Load())
	fmt.Fprintf(w, "bench_errors_total %d\n", c.errors.Load())
	fmt.Fprintf(w, "bench_bytes_sent_total %d\n", c.bytesSent.Load())
	fmt.Fprintf(w, "bench_bytes_received_total %d\n", c.bytesReceived.Load())
	fmt.Fprintf(w, "bench_connections_ready %d\n", c.connections.Load())
	fmt.Fprintf(w, "bench_connection_first_pass_seconds %.9f\n", float64(c.firstPassNanos.Load())/1e9)
	fmt.Fprintf(w, "bench_connection_setup_seconds %.9f\n", float64(c.setupNanoseconds.Load())/1e9)
	fmt.Fprintf(w, "bench_connection_setup_reconnects_total %d\n", c.setupReconnects.Load())
}

func serveMetrics(addr string, c *counters) (*http.Server, error) {
	if addr == "" {
		return nil, nil
	}
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/metrics", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		c.write(w)
	})
	server := &http.Server{Handler: mux}
	go func() { _ = server.Serve(listener) }()
	return server, nil
}

func transportConfig(tuned bool) *quic.Config {
	if !tuned {
		return &quic.Config{}
	}
	return &quic.Config{
		MaxIncomingUniStreams:          4096,
		MaxIncomingStreams:             4096,
		InitialStreamReceiveWindow:     4 << 20,
		MaxStreamReceiveWindow:         6 << 20,
		InitialConnectionReceiveWindow: 4 << 20,
		MaxConnectionReceiveWindow:     24 << 20,
		MaxIdleTimeout:                 15 * time.Second,
		KeepAlivePeriod:                5 * time.Second,
	}
}

func selfSignedCert() (tls.Certificate, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, err
	}
	template := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "ton-transport-bench"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		IPAddresses:  []net.IP{net.IPv4(127, 0, 0, 1)},
	}
	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, nil
}

func serverTLS() (*tls.Config, error) {
	cert, err := selfSignedCert()
	if err != nil {
		return nil, err
	}
	return &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{alpn}, MinVersion: tls.VersionTLS13}, nil
}

func clientTLS() *tls.Config {
	return &tls.Config{InsecureSkipVerify: true, NextProtos: []string{alpn}, MinVersion: tls.VersionTLS13}
}

func payload(size int, kind byte) []byte {
	data := make([]byte, size)
	data[0] = kind
	return data
}

func readPayload(r io.Reader, expected int, kind byte) ([]byte, error) {
	data, err := io.ReadAll(io.LimitReader(r, int64(expected+1)))
	if err != nil {
		return nil, err
	}
	if len(data) != expected || data[0] != kind {
		return nil, fmt.Errorf("bad payload: got %d bytes", len(data))
	}
	return data, nil
}

func handleUni(conn *quic.Conn, expected int, c *counters) {
	for {
		stream, err := conn.AcceptUniStream(context.Background())
		if err != nil {
			return
		}
		go func() {
			data, err := readPayload(stream, expected, messageByte)
			if err != nil {
				c.errors.Add(1)
				return
			}
			c.bytesReceived.Add(uint64(len(data)))
			c.messagesReceived.Add(1)
		}()
	}
}

func handleBidi(conn *quic.Conn, requestSize, responseSize int, c *counters) {
	response := payload(responseSize, responseByte)
	for {
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			return
		}
		go func() {
			data, err := io.ReadAll(io.LimitReader(stream, int64(requestSize+1)))
			if err != nil {
				c.errors.Add(1)
				return
			}
			if len(data) == 1 && data[0] == probeByte {
				if _, err := stream.Write([]byte{probeByte}); err != nil {
					c.errors.Add(1)
					return
				}
				_ = stream.Close()
				return
			}
			if len(data) != requestSize || data[0] != queryByte {
				c.errors.Add(1)
				return
			}
			c.bytesReceived.Add(uint64(len(data)))
			c.queriesReceived.Add(1)
			if _, err := stream.Write(response); err != nil {
				c.errors.Add(1)
				return
			}
			if err := stream.Close(); err != nil {
				c.errors.Add(1)
				return
			}
			c.bytesSent.Add(uint64(len(response)))
		}()
	}
}

func handleConnection(conn *quic.Conn, opts options, c *counters) {
	c.connections.Add(1)
	go handleUni(conn, opts.size, c)
	go handleBidi(conn, opts.size, opts.responseSize, c)
	<-conn.Context().Done()
	c.connections.Add(-1)
}

func runServer(ctx context.Context, opts options, c *counters) error {
	addr, err := net.ResolveUDPAddr("udp", opts.addr)
	if err != nil {
		return err
	}
	udp, err := net.ListenUDP("udp", addr)
	if err != nil {
		return err
	}
	transport := &quic.Transport{Conn: udp}
	tlsConfig, err := serverTLS()
	if err != nil {
		return err
	}
	listener, err := transport.Listen(tlsConfig, transportConfig(opts.tuned))
	if err != nil {
		return err
	}
	go func() {
		for {
			conn, err := listener.Accept(ctx)
			if err != nil {
				return
			}
			go handleConnection(conn, opts, c)
		}
	}()
	fmt.Println("READY server")
	<-ctx.Done()
	_ = listener.Close()
	_ = transport.Close()
	return nil
}

func sendProbe(conn *quic.Conn) error {
	stream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		return err
	}
	if _, err := stream.Write([]byte{probeByte}); err != nil {
		return err
	}
	if err := stream.Close(); err != nil {
		return err
	}
	data, err := io.ReadAll(io.LimitReader(stream, 2))
	if err != nil {
		return err
	}
	if len(data) != 1 || data[0] != probeByte {
		return fmt.Errorf("bad probe response")
	}
	return nil
}

func sendMessage(conn *quic.Conn, data []byte) error {
	stream, err := conn.OpenUniStreamSync(context.Background())
	if err != nil {
		return err
	}
	if _, err := stream.Write(data); err != nil {
		return err
	}
	return stream.Close()
}

func sendQuery(conn *quic.Conn, request []byte, responseSize int) error {
	stream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		return err
	}
	if _, err := stream.Write(request); err != nil {
		return err
	}
	if err := stream.Close(); err != nil {
		return err
	}
	_, err = readPayload(stream, responseSize, responseByte)
	return err
}

func consecutiveAddresses(base string, count int) ([]*net.UDPAddr, error) {
	host, portText, err := net.SplitHostPort(base)
	if err != nil {
		return nil, err
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port+count-1 > 65535 {
		return nil, fmt.Errorf("invalid server port range")
	}
	result := make([]*net.UDPAddr, count)
	for i := range result {
		result[i], err = net.ResolveUDPAddr("udp", net.JoinHostPort(host, strconv.Itoa(port+i)))
		if err != nil {
			return nil, err
		}
	}
	return result, nil
}

func connectAll(ctx context.Context, transport *quic.Transport, opts options) ([]*quic.Conn, uint64, time.Duration, error) {
	setupStart := time.Now()
	addresses, err := consecutiveAddresses(opts.serverAddr, opts.servers)
	if err != nil {
		return nil, 0, 0, err
	}
	connections := make([]*quic.Conn, opts.connections)
	jobs := make(chan int)
	errCh := make(chan error, 1)
	var workers sync.WaitGroup
	workerCount := min(opts.connectInflight, opts.connections)
	tlsConfig := clientTLS()
	quicConfig := transportConfig(opts.tuned)
	for range workerCount {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for index := range jobs {
				conn, err := connectOne(ctx, transport, addresses[index%len(addresses)], tlsConfig, quicConfig, index)
				if err != nil {
					select {
					case errCh <- fmt.Errorf("connection %d: %w", index, err):
					default:
					}
					return
				}
				connections[index] = conn
			}
		}()
	}
	go func() {
		defer close(jobs)
		started := time.Now()
		for i := range connections {
			if opts.connectRate > 0 && i != 0 {
				deadline := started.Add(time.Duration(float64(time.Second) * float64(i) / opts.connectRate))
				if delay := time.Until(deadline); delay > 0 {
					timer := time.NewTimer(delay)
					select {
					case <-timer.C:
					case <-ctx.Done():
						timer.Stop()
						return
					}
				}
			}
			select {
			case jobs <- i:
			case <-ctx.Done():
				return
			}
		}
	}()
	workers.Wait()
	select {
	case err := <-errCh:
		for _, conn := range connections {
			if conn != nil {
				_ = conn.CloseWithError(0, "setup failed")
			}
		}
		return nil, 0, 0, err
	default:
	}
	for i, conn := range connections {
		if conn == nil {
			return nil, 0, 0, fmt.Errorf("connection %d was not established", i)
		}
	}
	firstPassTime := time.Since(setupStart)
	reconnects, err := stabilizeConnections(ctx, transport, addresses, tlsConfig, quicConfig, connections)
	return connections, reconnects, firstPassTime, err
}

func stabilizeConnections(ctx context.Context, transport *quic.Transport, addresses []*net.UDPAddr,
	tlsConfig *tls.Config, quicConfig *quic.Config, connections []*quic.Conn) (uint64, error) {
	var reconnects uint64
	settle := time.Second
	if len(connections) >= 100_000 {
		settle = 15 * time.Second
	} else if len(connections) >= 10_000 {
		settle = 5 * time.Second
	}
	for round := 0; round != 10; round++ {
		timer := time.NewTimer(settle)
		select {
		case <-timer.C:
		case <-ctx.Done():
			timer.Stop()
			return reconnects, ctx.Err()
		}
		stable := true
		for index, conn := range connections {
			if conn.Context().Err() == nil {
				continue
			}
			stable = false
			replacement, err := connectOne(ctx, transport, addresses[index%len(addresses)], tlsConfig, quicConfig, index)
			if err != nil {
				return reconnects, fmt.Errorf("reconnect %d: %w", index, err)
			}
			connections[index] = replacement
			reconnects++
		}
		if stable {
			return reconnects, nil
		}
	}
	return reconnects, fmt.Errorf("connections did not stabilize after 10 rounds")
}

func connectOne(ctx context.Context, transport *quic.Transport, addr net.Addr, tlsConfig *tls.Config,
	quicConfig *quic.Config, index int) (*quic.Conn, error) {
	var lastErr error
	for attempt := 0; attempt != 5; attempt++ {
		conn, err := transport.Dial(ctx, addr, tlsConfig, quicConfig)
		if err == nil {
			if err = sendProbe(conn); err == nil {
				return conn, nil
			}
			_ = conn.CloseWithError(0, "readiness failed")
		}
		lastErr = err
		delay := time.Duration(20*(attempt+1)+index%17) * time.Millisecond
		timer := time.NewTimer(delay)
		select {
		case <-timer.C:
		case <-ctx.Done():
			timer.Stop()
			return nil, ctx.Err()
		}
	}
	return nil, lastErr
}

func runMessages(ctx context.Context, conns []*quic.Conn, opts options, c *counters) {
	data := payload(opts.size, messageByte)
	period := time.Duration(float64(time.Second) * float64(opts.burst) / float64(opts.rate))
	if period < time.Nanosecond {
		period = time.Nanosecond
	}
	next := time.Now()
	index := 0
	for {
		if delay := time.Until(next); delay > 0 {
			timer := time.NewTimer(delay)
			select {
			case <-ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			}
		} else {
			select {
			case <-ctx.Done():
				return
			default:
			}
		}
		for range opts.burst {
			if err := sendMessage(conns[index%len(conns)], data); err != nil {
				c.errors.Add(1)
			} else {
				c.messagesSent.Add(1)
				c.bytesSent.Add(uint64(len(data)))
			}
			index++
		}
		next = next.Add(period)
		now := time.Now()
		if now.Sub(next) >= period {
			next = now.Add(period)
		}
	}
}

func runQueries(ctx context.Context, conns []*quic.Conn, opts options, c *counters) {
	request := payload(opts.size, queryByte)
	var index atomic.Uint64
	for range opts.inflight {
		go func() {
			for {
				select {
				case <-ctx.Done():
					return
				default:
				}
				conn := conns[index.Add(1)%uint64(len(conns))]
				c.queriesSent.Add(1)
				if err := sendQuery(conn, request, opts.responseSize); err != nil {
					c.errors.Add(1)
					continue
				}
				c.bytesSent.Add(uint64(len(request)))
				c.bytesReceived.Add(uint64(opts.responseSize))
				c.queriesCompleted.Add(1)
			}
		}()
	}
}

// Mirrors bench-adnl-quic-message: a single process, one connection, closed-loop with a lag
// window, run to time. A quick one-command ceiling, not a harness workload.
const (
	maxReceiverLag    = 16 * 1024
	resumeReceiverLag = maxReceiverLag / 2
)

type saturateCounter struct {
	received atomic.Uint64
	invalid  atomic.Uint64
	target   atomic.Uint64

	mu      sync.Mutex
	reached chan struct{}
}

// A nil channel means the target was already reached.
func (c *saturateCounter) expect(target uint64) chan struct{} {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.received.Load() >= target {
		return nil
	}
	ch := make(chan struct{})
	c.reached = ch
	c.target.Store(target)
	return ch
}

func (c *saturateCounter) receive(size int, expected int) {
	if size != expected {
		c.invalid.Add(1)
		return
	}
	received := c.received.Add(1)
	if target := c.target.Load(); target == 0 || received < target {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if target := c.target.Load(); target == 0 || c.received.Load() < target {
		return
	}
	c.target.Store(0)
	if c.reached != nil {
		close(c.reached)
		c.reached = nil
	}
}

func waitFor(ch chan struct{}) {
	if ch != nil {
		<-ch
	}
}

func cpuSeconds() float64 {
	var ru syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &ru); err != nil {
		return 0
	}
	tv := func(t syscall.Timeval) float64 { return float64(t.Sec) + float64(t.Usec)/1e6 }
	return tv(ru.Utime) + tv(ru.Stime)
}

func runSaturate(opts options) error {
	serverUDP, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return err
	}
	serverTransport := &quic.Transport{Conn: serverUDP}
	tlsConfig, err := serverTLS()
	if err != nil {
		return err
	}
	listener, err := serverTransport.Listen(tlsConfig, transportConfig(opts.tuned))
	if err != nil {
		return err
	}
	c := &saturateCounter{}
	go func() {
		for {
			conn, err := listener.Accept(context.Background())
			if err != nil {
				return
			}
			go func() {
				for {
					stream, err := conn.AcceptUniStream(context.Background())
					if err != nil {
						return
					}
					go func() {
						data, err := io.ReadAll(io.LimitReader(stream, int64(opts.size+1)))
						if err != nil {
							c.invalid.Add(1)
							return
						}
						c.receive(len(data), opts.size)
					}()
				}
			}()
		}
	}()

	clientUDP, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return err
	}
	clientTransport := &quic.Transport{Conn: clientUDP}
	conn, err := clientTransport.Dial(context.Background(), serverUDP.LocalAddr(), clientTLS(), transportConfig(opts.tuned))
	if err != nil {
		return err
	}

	data := payload(opts.size, messageByte)

	// Warm up: one message, awaited, so the handshake is outside the measurement.
	warmup := c.expect(c.received.Load() + 1)
	if err := sendMessage(conn, data); err != nil {
		return err
	}
	waitFor(warmup)

	receivedAtStart := c.received.Load()
	cpuStart := cpuSeconds()
	var submitted uint64
	start := time.Now()
	for time.Since(start).Seconds() < opts.duration {
		if err := sendMessage(conn, data); err != nil {
			return err
		}
		submitted++
		received := c.received.Load() - receivedAtStart
		if submitted-min(submitted, received) >= maxReceiverLag {
			waitFor(c.expect(receivedAtStart + submitted - resumeReceiverLag))
		}
	}
	elapsed := time.Since(start).Seconds()
	received := c.received.Load() - receivedAtStart

	// Drain everything submitted before sampling CPU, so repeated runs are stable.
	waitFor(c.expect(receivedAtStart + submitted))
	cpu := cpuSeconds() - cpuStart

	if invalid := c.invalid.Load(); invalid != 0 {
		return fmt.Errorf("received %d invalid messages", invalid)
	}

	messagesPerSecond := float64(received) / elapsed
	var nsPerMessage, deliveryPercent, cpuUsPerMessage float64
	if received != 0 {
		nsPerMessage = elapsed * 1e9 / float64(received)
	}
	// CPU covers the drain as well, so it is charged against everything submitted, not against the
	// count sampled before the drain.
	if submitted != 0 {
		cpuUsPerMessage = cpu / float64(submitted) * 1e6
	}
	if submitted != 0 {
		deliveryPercent = 100 * float64(received) / float64(submitted)
	}
	fmt.Println("protocol=quic-go")
	fmt.Printf("threads=%d\n", opts.threads)
	fmt.Printf("payload_bytes=%d\n", opts.size)
	fmt.Printf("submitted=%d\n", submitted)
	fmt.Printf("received=%d\n", received)
	fmt.Printf("delivery_percent=%.3f\n", deliveryPercent)
	fmt.Printf("elapsed_seconds=%.3f\n", elapsed)
	fmt.Printf("ns_per_message=%.3f\n", nsPerMessage)
	fmt.Printf("messages_per_second=%.3f\n", messagesPerSecond)
	fmt.Printf("payload_MB_per_second=%.3f\n", messagesPerSecond*float64(opts.size)/1e6)
	fmt.Printf("cpu_seconds=%.3f\n", cpu)
	fmt.Printf("cpu_us_per_message=%.3f\n", cpuUsPerMessage)
	return nil
}

func runClient(ctx context.Context, opts options, c *counters) error {
	addr, err := net.ResolveUDPAddr("udp", opts.addr)
	if err != nil {
		return err
	}
	udp, err := net.ListenUDP("udp", addr)
	if err != nil {
		return err
	}
	transport := &quic.Transport{Conn: udp}
	start := time.Now()
	connections, reconnects, firstPassTime, err := connectAll(ctx, transport, opts)
	if err != nil {
		return err
	}
	c.connections.Store(int64(len(connections)))
	c.firstPassNanos.Store(uint64(firstPassTime))
	c.setupNanoseconds.Store(uint64(time.Since(start)))
	c.setupReconnects.Store(reconnects)
	for _, conn := range connections {
		conn := conn
		go func() {
			<-conn.Context().Done()
			c.connections.Add(-1)
			if ctx.Err() == nil {
				c.errors.Add(1)
			}
		}()
	}
	fmt.Printf("READY connections=%d setup_seconds=%.3f reconnects=%d\n", len(connections), time.Since(start).Seconds(), reconnects)
	switch opts.workload {
	case "message":
		go runMessages(ctx, connections, opts, c)
	case "query":
		runQueries(ctx, connections, opts, c)
	}
	<-ctx.Done()
	for _, conn := range connections {
		_ = conn.CloseWithError(0, "benchmark complete")
	}
	_ = transport.Close()
	return nil
}

func parseOptions() (options, error) {
	var opts options
	flag.BoolVar(&opts.server, "server", false, "run as server")
	flag.BoolVar(&opts.client, "client", false, "run as client")
	flag.StringVar(&opts.addr, "addr", "127.0.0.1:29320", "local UDP address")
	flag.StringVar(&opts.statsAddr, "stats-addr", "", "HTTP metrics address")
	flag.StringVar(&opts.serverAddr, "server-addr", "127.0.0.1:29320", "first server UDP address")
	flag.IntVar(&opts.servers, "servers", 1, "number of consecutive server addresses")
	flag.IntVar(&opts.connections, "connections", 1, "total client connections")
	flag.IntVar(&opts.connectInflight, "connect-inflight", 128, "concurrent connection setups")
	flag.Float64Var(&opts.connectRate, "connect-rate", 0, "maximum connection starts per second; 0 is unlimited")
	flag.StringVar(&opts.workload, "workload", "query", "idle, message, or query")
	flag.IntVar(&opts.size, "size", 240, "request or message bytes")
	flag.IntVar(&opts.responseSize, "response-size", 240, "query response bytes")
	flag.IntVar(&opts.rate, "rate", 4000, "aggregate messages per second")
	flag.IntVar(&opts.burst, "burst", 1, "messages per pacing event")
	flag.IntVar(&opts.inflight, "inflight", 8, "concurrent queries per client process")
	flag.IntVar(&opts.threads, "threads", 1, "Go scheduler threads")
	flag.BoolVar(&opts.tuned, "tuned", false, "use normalized flow control and idle timers")
	flag.Float64Var(&opts.duration, "duration", 5, "saturate workload measurement seconds")
	flag.Parse()
	if opts.workload == "saturate" {
		if opts.server || opts.client {
			return opts, fmt.Errorf("saturate runs both sides in one process; drop --server/--client")
		}
		if opts.duration <= 0 {
			return opts, fmt.Errorf("duration must be positive")
		}
		return opts, nil
	}
	if opts.server == opts.client {
		return opts, fmt.Errorf("choose exactly one of --server or --client")
	}
	if opts.threads < 1 || opts.servers < 1 || opts.connections < 1 || opts.connectInflight < 1 || opts.size < 1 || opts.responseSize < 1 {
		return opts, fmt.Errorf("counts and payload sizes must be positive")
	}
	if math.IsNaN(opts.connectRate) || math.IsInf(opts.connectRate, 0) || opts.connectRate < 0 {
		return opts, fmt.Errorf("connect rate must be finite and nonnegative")
	}
	if opts.client && opts.connections < opts.servers {
		return opts, fmt.Errorf("--connections must be at least --servers")
	}
	if opts.workload != "idle" && opts.workload != "message" && opts.workload != "query" {
		return opts, fmt.Errorf("unknown workload %q (idle, message, query, or saturate)", opts.workload)
	}
	if opts.workload == "message" && (opts.rate < 1 || opts.burst < 1) {
		return opts, fmt.Errorf("message rate and burst must be positive")
	}
	if opts.workload == "query" && opts.inflight < 1 {
		return opts, fmt.Errorf("query inflight must be positive")
	}
	return opts, nil
}

func run() error {
	opts, err := parseOptions()
	if err != nil {
		return err
	}
	runtime.GOMAXPROCS(opts.threads)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	c := &counters{}
	metricsServer, err := serveMetrics(opts.statsAddr, c)
	if err != nil {
		return err
	}
	if metricsServer != nil {
		defer metricsServer.Close()
	}
	if opts.workload == "saturate" {
		return runSaturate(opts)
	}
	if opts.server {
		return runServer(ctx, opts, c)
	}
	return runClient(ctx, opts, c)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
