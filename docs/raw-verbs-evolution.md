# Raw Verbs → rdma_cm: Why We Migrated

This project originally built RDMA connections using raw `libibverbs` and a custom OOB-over-TCP handshake. After three rounds of debugging on three different fabrics (SoftRoCE, Azure MANA, Alibaba eRDMA), we tore it all out and migrated to `librdmacm`. This doc records what we built, why it broke, and what the migration taught us.

The pre-migration code is recoverable from git history; the active codebase only has the rdma_cm path.

## What "raw verbs" meant in this project

To bring up an RC QP between two nodes, RDMA requires both sides to know each other's:
- QPN (Queue Pair Number)
- PSN (Packet Sequence Number)
- GID (Global Identifier — a 128-bit address, like an IPv6 addr but for RoCE)
- MR addr + rkey (for one-sided write/read)

The library doesn't provide a way to exchange these. You have to do it yourself, out-of-band. Our original design was:

```
1.  Each side opens its RDMA device, allocates PD + CQ
2.  Each side creates a QP — currently in RESET state
3.  Each side calls ibv_modify_qp to push QP to INIT
4.  Each side opens a TCP socket; server listens, client connects
5.  Both sides exchange {QPN, PSN, GID, MR addr, MR rkey} over the TCP socket
6.  Each side calls ibv_modify_qp twice more: INIT → RTR → RTS
    (RTR = Ready To Receive, RTS = Ready To Send)
7.  TCP socket closed. RDMA connection live.
```

This was implemented across:
- `rdma_context.c` — `rai_ctx_init` opens device, builds PD+CQ
- `rdma_qp.c` — `rai_qp_create` / `rai_qp_init` / `rai_qp_connect` walk the state machine
- `rdma_connect.c` — `oob_listen` / `oob_accept` / `oob_exchange_client` handle the TCP exchange

Total: ~250 lines of state-machine code, all of it removed in the migration.

## The four bugs that tipped us over

### Bug 1: GID selection on multi-GID devices

`ibv_query_gid(ctx, port, gid_index, &gid)` — but which `gid_index`? On SoftRoCE, GID 0 is link-local IPv6, GID 1 is global IPv4-mapped IPv6, etc. Pick the wrong one and `ibv_modify_qp(RTR)` succeeds but no packets ever reach the peer.

We hardcoded `gid_index = 1` for SoftRoCE. On Azure MANA the right index was different. On eRDMA the choice didn't matter because eRDMA doesn't even use GIDs (it's iWARP). Three fabrics, three rules. `librdmacm` figures this out itself via `rdma_resolve_route`.

### Bug 2: PSN sync

PSN (Packet Sequence Number) is the receiver's expected next-packet sequence. If sender's `local.psn` doesn't match what the receiver was told via `attr.rq_psn` during the RTR transition, packets are silently discarded — no error, no retry, just dead air.

Our code did `qp->local.psn = rand() & 0xffffff;` and exchanged it via OOB TCP. Worked, but felt fragile. `rdma_cm` handles PSN internally with no user involvement.

### Bug 3: RNR retry exhaustion on SoftRoCE

`lat_send_recv` would hang on SoftRoCE roughly 1 in 10 runs. The pattern:

1. Server enters its loop, calls `rai_post_recv()` to post a recv WR
2. Client immediately sends a message
3. **If the OS deschedules the server before `post_recv` finishes**, the client's message arrives before the recv WR is queued
4. Receiver returns RNR NAK ("Receiver Not Ready"); sender retries
5. Default `rnr_retry = 7`, default `min_rnr_timer ≈ 9 ms` per retry → ~63 ms total budget
6. On SoftRoCE with cloud VM scheduling jitter, the server can be paused for 100+ ms → all retries exhausted → `IBV_WC_RNR_RETRY_EXC_ERR` → connection dead

Two-part fix:
- In `rai_cm_server`: set `attr.min_rnr_timer = 31` (= 491 ms per retry, max value) immediately after `rdma_accept`. Multiplies the budget by ~50×.
- In `lat_send_recv`: pre-post one recv WR **before** `rdma_accept` returns, so the QP has a recv queued the instant the connection goes RTS. Eliminates the race entirely.

The pre-post pattern survives in the rdma_cm path (`rai_cm_server` still does it).

### Bug 4: eRDMA rejects manual ibv_modify_qp

This was the killer. Alibaba Cloud eRDMA is iWARP-based, not InfiniBand or RoCE. iWARP does the QP state machine in hardware — it does **not** accept user-driven `ibv_modify_qp(INIT)` / `(RTR)` / `(RTS)` calls.

Our raw verbs path called `ibv_modify_qp(qp, &attr, RTR_attrs)` and got back `errno=22 (EINVAL)`. There's no workaround at the verbs layer. The connection has to be brought up via `rdma_connect` / `rdma_accept`, which talks to the iWARP CM hardware directly.

**Bug 4 is what forced the migration**: bugs 1-3 had workarounds, bug 4 doesn't. If we wanted eRDMA support, we had to abandon the raw verbs path.

## What rdma_cm gives you for free

| Concern | Raw verbs | rdma_cm |
|---|---|---|
| Device + port discovery | You enumerate devices, query attrs, pick one | `rdma_resolve_addr` does it from the IP |
| GID selection | You guess the gid_index | `rdma_resolve_route` figures it out |
| PSN handshake | You exchange via OOB | Internal |
| QP state machine (INIT/RTR/RTS) | You write three `ibv_modify_qp` calls | `rdma_connect` / `rdma_accept` |
| iWARP support | Doesn't work | Works |

The CM model is more restrictive — you can't tweak QP attrs as freely — but the lost flexibility wasn't being used anywhere in this project. For an eRDMA target, it's not even optional.

## What we kept

- **OOB TCP** survives but with a smaller job: it now exchanges only MR `{addr, rkey}` info on `port + 1` for the Phase 2 transport's `exchange_buf()`. Connection setup is no longer its responsibility.
- **`rai_qp_destroy`** stayed and got upgraded to be idempotent — it now correctly handles a partially-initialized QP, so all `rai_cm_*` error paths can use it as a uniform cleanup hook.
- **Pre-post recv** survives as a defensive pattern in `rai_cm_server`. It's cheap insurance against the RNR race even though `min_rnr_timer = 31` makes the race much less catastrophic.

## What we threw away

```
rdma_context.c      — rai_ctx_init       (device open, manual selection)
rdma_qp.c functions — rai_qp_create      (manual ibv_create_qp + GID query)
                      rai_qp_init        (manual INIT transition)
                      rai_qp_connect     (manual RTR + RTS transitions)
```

The OOB TCP helpers (`rai_oob_listen / accept / connect`) survived but with a smaller scope — see "What we kept" above.

`rai_ctx_t` itself was eventually folded into `rai_qp_t` once it became clear the ctx/qp split was a holdover from the raw verbs design (where ctx held the manually-opened device). With rdma_cm, the device context is owned by the cm_id and never independently held — there's no role for a separate `ctx_t`.

## The lesson

The raw verbs path was educational — walking the QP state machine by hand made the protocol feel concrete in a way `rdma_connect` doesn't. But the production-grade verbs libraries (NCCL, UCX, libfabric) all use `rdma_cm` (or in NCCL's case, a custom CM that solves the same problems differently — see [NCCL's `net_ib.cc`](https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib.cc)). Manual state-machine walking is a learning artifact, not a production pattern.

If you want to replicate this evolution in your own learning project: start with raw verbs to understand the state machine, then migrate to rdma_cm before you have a production target. Don't do what we did — write 250 lines of fabric-portable state machine code, then realize Alibaba eRDMA doesn't accept any of it.
