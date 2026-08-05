package main

import (
	"context"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"testing/iotest"
	"testing/synctest"
	"time"

	"github.com/quic-go/quic-go"
)

func testCounter(t *testing.T) (context.Context, *saturateCounter) {
	t.Helper()
	ctx, stop := context.WithTimeout(context.Background(), time.Second)
	t.Cleanup(stop)
	ctx, fail := context.WithCancelCause(ctx)
	t.Cleanup(func() { fail(nil) })
	return ctx, &saturateCounter{fail: fail}
}

func TestSaturateReceive(t *testing.T) {
	for _, before := range []bool{false, true} {
		for _, name := range []string{"healthy", "short", "oversized", "read error"} {
			t.Run(name+map[bool]string{false: " after expect", true: " before expect"}[before], func(t *testing.T) {
				ctx, c := testCounter(t)
				var r io.Reader = strings.NewReader("good")
				switch name {
				case "short":
					r = strings.NewReader("bad")
				case "oversized":
					r = strings.NewReader("too long")
				case "read error":
					r = iotest.ErrReader(io.ErrUnexpectedEOF)
				}
				if before {
					c.receive(r, 4)
				}
				ch := c.expect(1)
				if !before {
					go c.receive(r, 4)
				}
				err := waitFor(ctx, ch)
				if name == "healthy" {
					if err != nil || c.received.Load() != 1 {
						t.Fatalf("healthy receive: count=%d error=%v", c.received.Load(), err)
					}
				} else if err == nil || errors.Is(err, context.DeadlineExceeded) || c.received.Load() != 0 {
					t.Fatalf("receive failure did not abort wait: count=%d error=%v", c.received.Load(), err)
				} else if name == "read error" && !errors.Is(err, io.ErrUnexpectedEOF) {
					t.Fatalf("lost read error: %v", err)
				}
			})
		}
	}
}

func TestSaturateWaitCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	for _, ch := range []chan struct{}{nil, make(chan struct{})} {
		if err := waitFor(ctx, ch); !errors.Is(err, context.Canceled) {
			t.Fatalf("cancellation: %v", err)
		}
	}
	ctx, cancel = context.WithTimeout(context.Background(), time.Millisecond)
	defer cancel()
	if err := waitFor(ctx, make(chan struct{})); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("deadline: %v", err)
	}
}

func TestSaturateProgressTimeout(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), progressTimeout+time.Second)
		defer cancel()
		err := waitFor(ctx, make(chan struct{}))
		if !errors.Is(err, context.DeadlineExceeded) || ctx.Err() != nil {
			t.Fatalf("progress deadline did not end the wait: %v", err)
		}
	})
}

func TestSaturateReceiverClosure(t *testing.T) {
	for _, connection := range []bool{false, true} {
		t.Run(map[bool]string{false: "listener", true: "connection"}[connection], func(t *testing.T) {
			ctx, c := testCounter(t)
			tlsConfig, err := serverTLS()
			if err != nil {
				t.Fatal(err)
			}
			listener, err := quic.ListenAddr("127.0.0.1:0", tlsConfig, transportConfig(true))
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			go c.accept(ctx, listener, 4)
			if connection {
				conn, err := quic.DialAddr(ctx, listener.Addr().String(), clientTLS(), transportConfig(true))
				if err != nil {
					t.Fatal(err)
				}
				// Await one received stream so the connection is accepted before closing it.
				ch := c.expect(1)
				if err := sendMessage(conn, []byte("good")); err != nil {
					t.Fatal(err)
				}
				if err := waitFor(ctx, ch); err != nil {
					t.Fatal(err)
				}
				_ = conn.CloseWithError(42, "test closure")
			} else {
				_ = listener.Close()
			}
			err = waitFor(ctx, c.expect(2))
			if err == nil || errors.Is(err, context.DeadlineExceeded) {
				t.Fatalf("receiver closure did not abort wait: %v", err)
			}
			if !connection && !errors.Is(err, net.ErrClosed) {
				t.Fatalf("lost listener error: %v", err)
			}
		})
	}
}

func TestRunSaturate(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := runSaturate(ctx, options{size: 128, duration: 0.02, threads: 1, tuned: true}); err != nil {
		t.Fatal(err)
	}
}

func TestRunSaturateCancellation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	start := time.Now()
	err := runSaturate(ctx, options{size: 128, duration: 60, threads: 1, tuned: true})
	if err == nil || ctx.Err() == nil || time.Since(start) > 2*time.Second {
		t.Fatalf("cancellation did not stop saturation promptly: %v", err)
	}
}
