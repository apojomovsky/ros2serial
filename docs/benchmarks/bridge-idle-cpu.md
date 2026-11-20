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
`/proc/<pid>/stat` twice over a thirty second window. The row is that
difference over the wall time, as a percentage of one core. The same run then
repeats with the firmware publishing one Float32 on `battery` ten times a
second, which is the ticket's second case.

Nothing here judges a bound: the numbers are for the write-up, and a bound in
the script would read as a gate a slower host could not tell from a
regression. The protocol's own limits, which this page does not judge, are in
[message bounds](../message-bounds.md). The 10 Hz rows above move between runs on a shared host, so the
idle figure and the delta against it are what the write-up reads.

## Figures

Two runs of each tree on one host, 30 s window, `utime + stime`:

| Tree | idle | 10 Hz | delta |
| --- | --- | --- | --- |
| `master` at `fc1bf3b2`, 1 ms timer | 13.53% | 12.80% | -0.73 |
| `master` at `fc1bf3b2`, 1 ms timer | 10.50% | 13.87% | +3.37 |
| #703, wake on readiness, 10 ms timer | 1.77% | 2.23% | +0.46 |
| #703, wake on readiness, 10 ms timer | 1.87% | 2.30% | +0.43 |

The 10 Hz case sits within a point of idle on the new tree, which is what the
ticket asks: forty bytes a second arrive as forty wakeups, and the work each
one costs is the frame it carries.

Where the old 10% went, as the design predicted and the drop confirms:
`Router::poll` ran the route, service and action repairs, took from every
service and action endpoint, and served the pacer, once per millisecond. The
receive half was nearly free on an idle line, because the port answered
`kWouldBlock` on the first read. Removing the receive half from the timer and
slowing the timer to a period its own deadlines need (the shortest is the
500 ms heartbeat) is what moved the figure, not any change inside the receive
path itself.
