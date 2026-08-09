# Benchmark mistakes and how they were caught

The README has the numbers that held up. Here are the ones that did not. Nearly
all of them were caught the same way: run perftest at the same point, and ask
why the two disagree. It found as many errors in the measurement as in the code.

## The two-sided path was half the speed of perftest

First run of the size sweep, at 4 KB:

| | one-way µs |
|---|---|
| this implementation, send/recv | 3.67 |
| `ib_send_lat` | 1.85 |
| this implementation, write | 1.96 |
| `ib_write_lat` | 1.80 |

The write path matched. Only send/recv was off, and by a factor of two — so it
was not the link, the GID, or the machine. Something in the receive path.

**First hypothesis: the send completion.** Every send here was posted with
`IBV_SEND_SIGNALED` and its completion polled before the loop moved on. An RC
send only completes once the peer acknowledges, so that put a wire round trip
on the critical path. perftest signals one send in every hundred
(`cq_mod = 100`) and never waits on the rest.

Testing it meant adding a knob for the signalling interval. Set to 100:
7.79 → 7.33 µs RTT. **A 6% effect, against a gap of about 100%.** Wrong
hypothesis, and the knob came back out afterwards.

**Second hypothesis: receive queue depth.** Reading `ctx_set_recv_wqes` in
perftest: it pre-posts `rx_depth` receive work requests, 512 by default. This
implementation kept exactly one posted, replacing it after each arrival. An
arriving message needs a receive WQE before the NIC can place the payload; with
one just-posted WR the NIC has to fetch it from host memory, and that fetch sits
on the critical path.

Pre-posting a batch: **7.81 → 3.89 µs RTT**, within 5% of perftest.

The confirmation was worth more than the fix. perftest exposes the same setting
as `-r`, so it could be swept too — and it slows down by the same amount,
1.85 → 3.57 µs one-way at depth 1. Whatever causes this sits below both
programs, in the driver or the card. The one-sided write benchmark, which posts
no receive work requests at all, does not move.

## Three measurements that had to be thrown away

- **1 KB bandwidth, over in 0.34 ms.** A fixed 1000 iterations is 1 MB of
  traffic at that size — all pipeline fill, no steady state. Scaling so every
  point moves 256 MB moved perftest further than it moved this implementation
  (19.9 → 28.7 Gbps): the short window had been penalising the reference.
- **perftest given `-t 16` to match `--depth 16`.** It pairs that with its own
  `cq_mod = 100`, and 16 outstanding against a completion every hundred is a
  corner it handles badly. Left at its own default it does 62 Gbps against this
  implementation's 50 — it wins. Sub-16 KB bandwidth is now reported as not
  comparable rather than as a result.
- **2.2% run-to-run spread, and one 64 KB point swinging 36%**, from a
  `schedutil` governor parking busy-waiting cores at 1.2 GHz. Small messages are
  message-rate bound, so that is exactly where it lands; large transfers are
  link-bound and never noticed. Pinned to `performance`, spread drops to 0.5%.

Each of these first showed up as this implementation beating perftest.

## A teardown race at the end of the bandwidth test

`bw_rdma_write` at 64 KB failed with `IBV_WC_RETRY_EXC_ERR` — the client
retransmitting into a peer that had stopped answering.

The server exits when it sees the last write's doorbell byte. But a doorbell
means the *data* landed; RC may still owe acknowledgements for the unsignaled
writes queued behind it. The server tore the queue pair down inside that window
and the client retried into nothing. The deeper the pipeline the likelier this
gets, since depth is exactly how many writes can still be unacknowledged when
the loop ends.

perftest has the same barrier, with the reason in the comment
(`write_bw.c`): *"For half duplex tests, server just waits for client to exit."*

## Depth meant two things at once

`bw_rdma_write` posted `--depth` writes, blocked for a completion, then posted
the next batch. So one setting controlled two things: how many writes could be
in flight, and how often the pipeline drained to empty. And draining is not
free — a send completion only arrives once the peer acknowledges. At depth 2
that is a stall every two messages:

| depth | before | after | `ib_write_bw` |
|---|---|---|---|
| 1 | 59.22 | 59.18 | 58.00 |
| 2 | 70.77 | **92.40** | 92.39 |
| 4 | 80.38 | 92.36 | 92.49 |
| 16 | 88.84 | 92.12 | 92.47 |

Posting against a window and reaping without blocking — what perftest's
`run_iter_bw` does — closed it. The 16 KB point in the size sweep, which had
been sitting at 83% of perftest for the same reason, came up with it.

Depth 1 is the control: with a window of one, old and new are the same
algorithm, and it did not move.
