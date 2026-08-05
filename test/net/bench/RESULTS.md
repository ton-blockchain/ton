# Reference results — Linux, 2026-08-05

Xeon Gold 6548N (32 cores), Debian 6.1, loopback, RelWithDebInfo, `TON_USE_JEMALLOC=ON`,
ngtcp2 v1.22.0, quic-go v0.61.0, quinn 0.11 (native drivers: parity profile, 1 thread).
The host carried ~3 cores of unrelated load; treat numbers as ±10%. Everything below is
verbatim script output, so a rerun can be diffed against this file.

## Suite

`IMPLEMENTATIONS="ton quic-go quinn" PROTOCOLS="quic adnl rldp2" REPETITIONS=3 suite.sh`:

```
scenario     impl     proto  delivered/s  app MiB/s  cpu us/msg  peer cores  dgram/msg  wire B/msg  status
ihave-1x20   ton      quic   4000         0.9        25.49       0.36        1.501      359         ok
ihave-1x20   quic-go  quic   3999         0.9        18.11       0.34        -          -           ok
ihave-1x20   quinn    quic   4000         0.9        10.49       0.16        -          -           ok
ihave-1x20   ton      adnl   4000         0.9        16.11       0.19        1.000      364         ok
ihave-1x20   ton      rldp2  4000         0.9        80.06       0.54        2.000      2092        ok
ihave-40x20  ton      quic   6000         1.4        5.33        0.13        0.246      289         ok
ihave-40x20  quic-go  quic   6001         1.4        5.33        0.10        -          -           ok
ihave-40x20  quinn    quic   6001         1.4        2.00        0.03        -          -           ok
ihave-40x20  ton      adnl   6000         1.4        11.33       0.06        0.999      364         ok
ihave-40x20  ton      rldp2  5995         1.4        72.51       0.50        2.350      2486        ok
bulk-common  ton      quic   29.4k        244.4      31.65       1.46        1.526      9107        ok
bulk-common  quic-go  quic   27.0k        224.3      37.07       1.29        -          -           ok
bulk-common  quinn    quic   54.9k        455.4      18.22       1.33        -          -           ok
bulk-common  ton      adnl   9590         79.6       108.83      1.50        2.000      10444       ok
bulk-common  ton      rldp2  1720         14.3       377.93      0.78        8.622      19994       ok
bulk-large   ton      quic   3887         972.0      259.83      1.25        188.775    271638      ok
bulk-large   quic-go  quic   2124         531.2      433.05      1.82        -          -           ok
bulk-large   quinn    quic   4262         1065.7     235.06      1.36        -          -           ok
bulk-large   ton      rldp2  280          70.0       4285.52     1.05        352.662    343324      ok
fanin        ton      quic   8000         1.8        31.86       0.68        0.955      358         ok
fanin        quic-go  quic   7999         1.8        27.73       0.45        -          -           ok
fanin        quinn    quic   7998         1.8        15.80       0.24        -          -           ok
fanin        ton      adnl   7998         1.8        18.99       0.43        0.001      364         ok
fanin        ton      rldp2  7995         1.8        53.56       1.23        1.000      2092        ok
sat-1x1      ton      quic   633.2k       144.9      1.77        1.92        0.025      288         2/3 ok
sat-1x1      quic-go  quic   127.2k       29.1       7.80        0.70        -          -           ok
sat-1x1      quinn    quic   84.3k        19.3       1.91        0.10        -          -           ok
sat-1x1      ton      adnl   165.4k       37.9       5.45        1.29        0.000      364         ok
sat-20x400   ton      quic   451.4k       103.3      3.85        4.33        0.241      287         ok
sat-20x400   quic-go  quic   207.8k       47.6       4.83        2.29        -          -           ok
sat-20x400   quinn    quic   244.1k       55.9       1.36        0.90        -          -           ok
sat-20x400   ton      adnl   157.7k       36.1       7.98        1.11        1.000      364         ok

idle (established connections doing nothing; cores and RSS are the result)
scenario    impl     proto  peers  cores   RSS MiB  KiB/peer  wire dgram/s  status
idle-small  ton      quic   32     0.0000  66.0     201.4     16.2          ok
idle-small  quic-go  quic   32     0.0010  15.1     254.6     -             ok
idle-small  quinn    quic   32     0.0005  23.6     710.8     -             ok
idle-small  ton      adnl   32     0.0005  59.4     0.0       0.0           ok
idle-small  ton      rldp2  32     0.0000  61.9     7.9       0.0           ok
```

Native saturation rows are bounded by each driver's own pacing loop, not its transport;
the single-process ceilings below are the transport-only numbers. One `sat-1x1 ton quic`
repetition was INVALID (delivery dipped below 99% near the ceiling); medians span the rest.

## Same suite at 50 ms RTT (netem in a network namespace; TON only)

```
scenario           impl  proto  delivered/s  app MiB/s  cpu us/msg  peer cores  dgram/msg  wire B/msg  status
rtt50-ihave-1x20   ton   quic   4000         0.9        18.24       0.31        1.079      318         ok
rtt50-ihave-40x20  ton   quic   6001         1.4        6.00        0.12        0.239      288         ok
rtt50-sat-1x1      ton   quic   354.7k       81.2       1.87        1.68        0.016      289         ok
```

Sparse fan-out drops from 1.50 to 1.08 datagrams/message: the loopback excess is ngtcp2's
ack+PING liveness rule at sub-millisecond srtt, gone at real RTT. The saturation row needs the
lifted stream-credit window (`--no-limits`); the default 4096 streams cap one connection at
roughly 80k msg/s at this RTT.

## Transport-only ceilings (single process, closed loop; exact stdout)

```
$ bench-adnl-quic-message --quic -d 5 -t 1
protocol=quic
threads=1
split_schedulers=0
receiver_lag=16384
payload_bytes=240
submitted=1934640
received=1926449
delivery_percent=99.577
elapsed_seconds=5.005
ns_per_message=2597.877
messages_per_second=384929.754
payload_MB_per_second=92.383
cpu_seconds=8.878
cpu_us_per_message=4.589

$ bench-adnl-quic-message --quic --quic-datagrams -d 5 -t 1
protocol=quic
threads=1
split_schedulers=0
receiver_lag=2048
payload_bytes=240
submitted=3983313
received=3983207
delivery_percent=99.997
elapsed_seconds=5.000
ns_per_message=1255.388
messages_per_second=796566.186
payload_MB_per_second=191.176
cpu_seconds=8.560
cpu_us_per_message=2.149

$ bench-adnl-quic-message --adnl -d 5 -t 1
protocol=adnl
threads=1
split_schedulers=0
receiver_lag=16384
payload_bytes=240
submitted=475136
received=475136
delivery_percent=100.000
elapsed_seconds=5.175
ns_per_message=10891.786
messages_per_second=91812.304
payload_MB_per_second=22.035
cpu_seconds=6.195
cpu_us_per_message=13.039

$ quicmsgbench-go -workload saturate -duration 5 -tuned
protocol=quic-go
threads=1
payload_bytes=240
submitted=569332
received=566558
delivery_percent=99.513
elapsed_seconds=5.008
ns_per_message=8838.559
messages_per_second=113140.609
payload_MB_per_second=27.154
cpu_seconds=4.953
cpu_us_per_message=8.742

$ quicmsgbench-rs --workload saturate -d 5 --tuned
protocol=quinn
threads=1
payload_bytes=240
submitted=2301886
received=2299277
delivery_percent=99.887
elapsed_seconds=5.000
ns_per_message=2174.685
messages_per_second=459836.795
payload_MB_per_second=110.361
cpu_seconds=6.138
cpu_us_per_message=2.669
```
