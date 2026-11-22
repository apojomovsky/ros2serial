# Bridge idle CPU

Question: how much of one core does `r2s_bridge` burn while its link is
quiet, and does the receive path cost anything per byte that arrives? The
node used to sample the port on a wall timer, so an idle line still paid for
a poll pass a thousand times a second, and a byte waited for the next pass
before anything read it.

## Method

`make bench-bridge` runs `benchmark/bridge/run.sh`, which builds the bridge
and the posix firmware from the same tree, joins them with a pty pair, and
runs `benchmark/bridge/idle_cpu.py`. The firmware is told to publish nothing
(`--no-pub --no-echo --no-time-now --no-joints --no-log-text`), so the link
carries only the handshake and the heartbeats the bridge sends itself.

The script waits for `/diagnostics` to report a connected link, settles for
five seconds so the handshake burst and the first corrections land outside
the measurement, then reads the bridge's own `utime + stime` from
`/proc/<pid>/stat` twice over the run's window (thirty seconds unless
`run.sh --window` says otherwise). The row is that
difference over the wall time, as a percentage of one core. The same run then
repeats with the firmware publishing one Float32 on `battery` ten times a
second, which is the ticket's second case.

Nothing here judges a bound: the numbers are for the write-up, and a bound in
the script would read as a gate a slower host could not tell from a
regression. The protocol's own limits, which this page does not judge, are in
[message bounds](../message-bounds.md). The 10 Hz rows above move between runs on a shared host, so the
idle figure and the delta against it are what the write-up reads.

## Figures

Runs on one host, `utime + stime`, each row naming its window:

| Tree | idle | 10 Hz | delta |
| --- | --- | --- | --- |
| `master` at `fc1bf3b2`, 1 ms timer, 30 s | 13.53% | 12.80% | -0.73 |
| `master` at `fc1bf3b2`, 1 ms timer, 30 s | 10.50% | 13.87% | +3.37 |
| #703, wake on readiness, 10 ms timer, 30 s | 2.23% | 2.53% | +0.30 |
| #703, wake on readiness, 20 ms timer, 60 s | 1.15% | 1.45% | +0.30 |
| #703, wake on readiness, 100 ms timer, 30 s | 0.37% | 0.70% | +0.33 |

The 10 Hz case sits within a point of idle on the new tree, which is what the
ticket asks: forty bytes a second arrive as forty wakeups, and the work each
one costs is the frame it carries.

The 10 ms row is why the period is 20 ms and not 10: the merged tree reads
2.23% idle at 10 ms, above the ticket's 2%, while the pre-merge tree had read
1.77 to 1.87% at the same period. The bridge hot path is unchanged between
the two, so the write-up reads the move as host drift and takes the margin
instead of the estimate. The 100 ms row is a probe, not a candidate: it shows
the cost scales with the timer rate, which is what makes the period the dial.

Where the old 10% went, as the design predicted and the drop confirms:
`Router::poll` ran the route, service and action repairs, took from every
service and action endpoint, and served the pacer, once per millisecond. The
receive half was nearly free on an idle line, because the port answered
`kWouldBlock` on the first read. Removing the receive half from the timer and
slowing the timer to a period its own deadlines need (the shortest is the
500 ms heartbeat) is what moved the figure, not any change inside the receive
path itself.
