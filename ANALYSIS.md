# Peer-to-Peer Distributed File Sharing System — Design & Algorithm Analysis

A BitTorrent-inspired file sharing system in C++ (Linux, POSIX sockets, OpenSSL EVP)
with a fault-tolerant tracker, parallel multi-peer downloads, pluggable piece
selection strategies, partial seeding, and end-to-end SHA-1 integrity.

```
 Repository layout
 ├── common/            protocol framing + SHA-1 hashing (shared by tracker & client)
 ├── tracker/           state machine, replication manager, client-facing server
 ├── client/            line-editor CLI, download manager, peer server, piece selectors
 └── tests/             48 unit tests (ASan/UBSan) + 4-scenario integration harness
```

---

## 1. Executive summary

| Aspect | Summary |
|---|---|
| **Model** | Hybrid P2P: a tracker coordinates metadata only; file bytes flow peer-to-peer. |
| **Language / deps** | C++17, Linux POSIX sockets, OpenSSL EVP (SHA-1). No frameworks, no external runtime deps. |
| **Transport** | TCP with a 4-byte length-prefixed message framing layer. |
| **Piece size** | 512 KB, each verified by SHA-1 before being accepted. |
| **Concurrency** | Threaded: one worker per connected peer, scheduler thread per download, one thread per tracker client, dedicated replication thread. |
| **Fault tolerance** | Primary/backup tracker with automatic promotion and state replication. |
| **Verified by** | 48 dependency-free unit tests under AddressSanitizer + UBSan, plus an integration harness covering transfers, multi-peer downloads, live tracker failover, and mid-download corruption. |

---

## 2. Architecture

### 2.1 The core idea: control plane / data plane split

The single most important design decision is the separation of *who knows what*
from *who has the data*:

```
                        CONTROL PLANE (metadata only)
        ┌────────────────────── Tracker (Primary) ──────────────────────┐
        │  users · groups · file metadata · seeder/leecher lists        │
        │  passwords · piece-hash manifests                             │
        └────────┬───────────────────────────────────────────┬──────────┘
   replication   │ upload_file / download_file                │ same protocol
   (port+1000)   ▼                                            ▼
        ┌──────────────────┐    IP:PORT lists      ┌──────────────────┐
        │ Tracker (Backup) │◀──── state sync ──────│      Client A    │
        └──────────────────┘                       └───────┬──────────┘
                                                            │ 1. get seeder list
                                                            │ 2. connect peer-to-peer
                                             DATA PLANE     ▼
                                              ┌──────────────────┐
                     HANDSHAKE → BITFIELD ───▶│      Client B    │
                     REQUEST_PIECE ⇄ PIECE    │  (seeder, 512KB  │
                                              │   SHA-1 chunks)  │
                                              └──────────────────┘
```

The tracker **never touches file bytes**. It stores only:

```cpp
// tracker/include/tracker_state.h (abridged)
struct FileMetadata {
  std::string concatenated_hashes;      // ordered SHA-1 chain of all pieces
  std::set<std::string> seeders;        // user IDs currently sharing
  std::set<std::string> leechers;       // user IDs currently downloading
};
struct GroupInfo {
  std::string owner_id;
  std::set<std::string> members;
  std::set<std::string> pending_requests;   // owner-approval join model
  std::unordered_map<std::string, FileMetadata> files;
};
```

Consequences of this split:

* The tracker is small-state, cheap, and *replicable* (Section 5).
* Download throughput scales with peers, not with tracker capacity.
* Metadata (hash manifest) is the single source of truth used later for
  cryptographic verification of every byte (Section 4.3).

### 2.2 Process model

| Process | Threads |
|---|---|
| **Tracker** | main accept loop (`select`, 1 s tick) · one `handle_client` thread per connected client · replication thread (election/sync/heartbeat) · console thread (`quit`) |
| **Client** | main raw-mode input loop (`select` on stdin + tracker socket) · download orchestrator per active download · one `peer_worker` thread per remote peer · peer-server accept loop · per-download completion/upload hooks |

Client-side console state (`current_user_id`, `is_logged_in`) is guarded by
atomics; download state by a dedicated `state_mutex` plus a separate
`file_mutex` for disk writes — lock ordering is always state → file, avoiding
deadlock between piece bookkeeping and I/O.

---

## 3. Wire protocol

All TCP streams are framed by a 4-byte length prefix (`common/protocol.h`):
`sendMessage` prepends the length; `receiveMessage` blocks until exactly that
many bytes arrive. This eliminates TCP's message-boundary problem and keeps
every command handler simple.

### 3.1 Client ↔ Tracker (control plane)

| Command | Purpose |
|---|---|
| `create_user <id> <pw>` | register |
| `login` / `logout` | session (per-connection, invalidated on tracker failover) |
| `create_group` / `join_group` / `leave_group` | group lifecycle (owner-approval joins) |
| `accept_request <g> <u>` | owner approves a pending join |
| `list_groups` / `list_requests` / `list_files` | discovery |
| `upload_file <g> <name> <size> <concat-hashes>` | register as seeder + publish hash manifest |
| `start_downloading` / `stop_leeching` | enter/leave the leecher set |
| `stop_share` | stop seeding a file |
| `download_file <g> <name>` | **returns the seeder `ip:port` list + hash manifest** |

Every argument is validated (arity, integer parsing via exception-guarded
`stoll`, non-negative sizes, membership and ownership checks), and unknown
commands produce a structured `ERROR: Unknown command '<cmd>'` — the client
prints these without crashing.

### 3.2 Peer ↔ Peer (data plane)

```
leecher                                seeder
  │── HANDSHAKE <file_name> ────────────▶│  (file must be known locally)
  │◀───────────────────── HANDSHAKE_OK ──│
  │◀── BITFIELD <'1'/'0' per piece> ─────│  actual possession, not "all 1s"
  │── REQUEST_PIECE <idx> ──────────────▶│
  │◀── PIECE <idx> <512KB raw bytes> ────│
  │   (SHA-1 checked locally; retry on mismatch)
```

Two protocol-level hardening details:

* The seeder's bitfield reports **what it actually has** — a fully seeded file
  yields all `1`s, a partially downloaded file yields its true per-piece
  vector. This is what enables partial seeding (Section 4.4).
* `PIECE` responses are parsed positionally (payload begins after the second
  space) and the echoed index must match the request — a stale or malformed
  response can never be written to the wrong file offset.
* The seeder rejects out-of-range piece indices (`piece_index` vs
  `ceil(file_size / PIECE_SIZE)`) and never reads beyond EOF.

---

## 4. Algorithms in depth

### 4.1 Piece selection — pluggable strategy pattern

Selection is abstracted behind one interface, making algorithms swappable at
runtime (`download_file <g> <f> <dest> <alg>`):

```cpp
class PieceSelector {
 public:
  virtual int select_piece(DownloadState& state) = 0;  // -1 = nothing available
};
```

Shared state every strategy consults (all under `state_mutex`):

```
pieces_we_have[]      acquired pieces
pieces_in_progress[]  currently assigned to some peer
pieces_exhausted[]    failed MAX_PIECE_ATTEMPTS (5) times — blacklisted
piece_rarity[]        count of active peers holding each piece
```

#### Rarest-first (default)

```
select_piece:
    best ← -1,  min_rarity ← |peers| + 1
    for each piece i not in (have ∪ in_progress ∪ exhausted):
        if rarity[i] < min_rarity:
            if any active peer's bitfield[i]:      # must be servable now
                min_rarity ← rarity[i],  best ← i
    return best
```

*Complexity:* O(P × S) per selection (P pieces, S peers), P ≤ file/512 KB so
this is trivially fast in practice.

**Why rarest-first matters** (the same reasoning as BitTorrent): the rarest
pieces are the ones closest to disappearing if their holders leave. Downloading
them first converts the swarm's most fragile knowledge into widely-replicated
data. It also naturally diversifies which pieces different leechers fetch,
increasing the number of useful partial seeders for everyone else. A final
boundary case is handled explicitly: a piece is only returned if at least one
*currently active* peer's bitfield actually contains it, so selection can
never dead-end on a peer that dropped out.

#### Sequential — `0, 1, 2, …`
Lowest-index servable piece first. Useful for streaming-style access (early
pieces arrive in order); worst case for swarm health since every leecher
contends for the same prefix.

#### Random — uniform over servable pieces
Available pieces are collected and shuffled (time-seeded
`std::default_random_engine`). Good as a contention-spreading baseline and as
a fuzz-ish stress input for the scheduler.

Unknown algorithm names are rejected with a warning and fall back to
rarest-first — the CLI can never be tricked into a null strategy.

### 4.2 Download orchestration — a centralized work-stealing scheduler

`start_download` runs a coordinator loop; each connected peer runs its own
`peer_worker` thread that sleeps on a condition variable until assigned work:

```
start_download:                                peer_worker (per peer):
  pick strategy                                  handshake → BITFIELD
  open output file (O_TRUNC)                     update rarity[], go idle
  spawn one worker thread per peer               loop:
  wait until all workers report ready              wait(cv: assigned_piece != -1
  while downloaded < num_pieces:                        OR download_complete)
    for each idle active peer p:                   send REQUEST_PIECE
      idx ← select_piece()                         receive PIECE
      if idx servable by p:                        verify SHA-1 vs manifest
        in_progress[idx] ← true                    pwrite(fd, data, offset)
        p.assigned_piece ← idx                         = size → success
    if any assignment: cv.notify_all()             else → failure path
    else if nothing in flight anywhere: ABORT      (see below)
    sleep(100 ms)
  final whole-file hash verification
```

Key correctness and liveness properties:

* **No busy-waiting per worker** — idle peers block on a condition variable;
  only the coordinator sleeps on a timer.
* **Completion is progress-gated.** If `select_piece()` returns `-1` for every
  idle peer *and* no piece is in flight, the download aborts with a clear
  error instead of spinning forever. This is the distinction between "temporarily
  starved (someone is still fetching)" and "permanently unservable (nobody has
  the remaining pieces)".
* **Writes are atomic seeks.** Pieces land via `pwrite(output_fd, data,
  piece_index * PIECE_SIZE)` under `file_mutex` — a piece counts as downloaded
  only if the *entire* payload reached the disk, so short writes are retried
  rather than silently corrupting the file.

### 4.3 Integrity — two-layer SHA-1 verification

Layer 1 — **per piece, on receipt**: `hash_buffer()` (OpenSSL EVP SHA-1) is
computed over the received bytes and compared against the publisher's
manifest entry. Mismatches are rejected *before* any disk write and counted
as a failed attempt:

* after **5** failures (`MAX_PIECE_ATTEMPTS`) a piece is blacklisted
  (`pieces_exhausted`), so one persistently corrupt or flaky peer cannot stall
  a download forever — the coordinator simply never selects that piece again,
  and if nothing else can serve it, the abort rule of 4.2 ends the download
  with a clear diagnosis.

Layer 2 — **whole file, at the end**: the completed file is re-hashed
piece-by-piece (`compute_hashes`) and the concatenated digest is compared with
the manifest. This catches any corruption path that could bypass layer 1
(disk-level error after a "successful" write, parsing bug, offset math error).
Only after layer 2 passes does the downloader announce the file and (via the
tracker) promote itself from leecher to seeder.

The publisher pays a symmetric cost at upload time: `compute_hashes` reads the
file incrementally (constant memory, regardless of file size) and registers
`concatenated_hashes` with the tracker.

### 4.4 Partial seeding — swarm acceleration

A leecher is simultaneously a server for the pieces it already holds. Its
peer-server builds bitfields from live download state:

```cpp
// client_peer.cpp (abridged)
if (is_partial && download_state) {
  for (bool have : download_state->pieces_we_have)   // true possession
    bitfield += have ? '1' : '0';
} else {
  bitfield = std::string(num_pieces, '1');            // complete seeder
}
```

Combined with rarest-first selection this creates a positive feedback loop:
early pieces become available from many sources while downloads are still
running, raising aggregate throughput well beyond what "download only from
original seeders" would allow. When a leech finishes, its already-verified
pieces mean it becomes a full seeder without re-verification; the client then
re-issues `upload_file` so the tracker's seeder set reflects reality.

### 4.5 Tracker replication & failover — Primary/Backup

Two trackers, N = `tracker_info.txt` entries, replication listener on
`client_port + 1000`:

**Role election (each tick of `ReplicationManager::run`):**

```
if N == 1:  become PRIMARY immediately            # no peer to elect with
else:
    connect to the OTHER tracker's replication port (2 s timeout,
    non-blocking connect + select so a dead peer can't hang us)
    connect succeeded → I am BACKUP   (someone else is primary)
    connect refused   → I am PRIMARY   (no active primary out there)
```

This is a deliberate simplification: with exactly two trackers, "the other one
isn't a functioning primary" and "there is no primary" coincide, so no voting
quorum is needed. A split-brain is impossible because a tracker only assumes
backup status by *observing* a live primary, and only claims primary status
when it *cannot observe* one.

**State synchronization:**

* On becoming backup, the primary streams `FULL_STATE_SYNC` — the entire
  `TrackerState` serialized (`USER`, `GROUP`, `FILE …` records) — then flushes
  its queue of commands that fired while the backup was down, then clears it.
* While healthy, every mutating command is additionally forwarded verbatim
  (`forward_command`) and replayed by the backup via
  `process_replicated_command`. The queue is capped at 1000 entries; overflow
  forces reliance on the next full sync rather than unbounded memory.
* The primary sends `HEARTBEAT` every 2 s on the replication socket; a failed
  send immediately demotes the link and re-enters the wait-for-backup state.

**Failover (backup side):** if `receiveMessage` from the primary ever fails,
the backup promotes itself, **clears all client sessions** (login state is
per-connection and cannot be safely preserved), rebinds the client-facing
port, and serves new connections from its replicated state. Failover is
bounded by the 2 s connect timeout / 2 s heartbeat rather than by TCP's
default multi-minute blocking `connect()` — the backup's connect loop is
explicitly interruptible by `should_stop`.

Client side, the failover is mostly transparent: clients hold both tracker
addresses and retry down the list at startup (`Trying next...`), and the
download/leech state is restored by re-issuing `upload_file`/`stop_leeching`
as needed.

### 4.6 Concurrency model summary

| Shared state | Protection |
|---|---|
| Download piece bookkeeping (`pieces_*`, `piece_rarity`) | `state_mutex` + condition variable |
| Output file writes | `file_mutex` + `pwrite` at fixed offsets |
| Console I/O | `cout_mutex` + redraw-above-prompt line discipline |
| Login state | `std::atomic<bool>` |
| Tracker state (users/groups/files) | `TrackerState` internal mutex; replication queue guarded separately |
| Piece assignment | single coordinator thread assigns; workers only clear their own slot |

---

## 5. Feature catalog

### Groups & sharing
* **Owner-approval groups** — `create_group` makes you owner; `join_group`
  lands in `pending_requests`; only `accept_request` from the owner moves a
  user into `members`. Every file operation validates membership.
* **Per-group file namespace** — `FileMetadata` is keyed by logical name
  inside its group; `stop_share` and `stop_leeching` update the seeder/
  leecher sets so discovery stays truthful.

### Download engine
* **Parallel multi-peer downloads** — one thread per seeder, coordinated by
  the scheduler of 4.2; S2 of the integration harness proves a client pulls
  from two seeders simultaneously.
* **Pluggable piece strategies** — rarest-first (default) / sequential /
  random, selectable per download; unknown names fail safe to rarest-first.
* **Per-peer I/O timeouts** (10 s) — a peer that accepts but never replies
  cannot wedge a worker; the piece returns to the selectable pool.
* **Retry budget & blacklist** — 5 attempts per piece (4.3), plus the
  no-progress abort rule (4.2), give every download a guaranteed termination
  path: success, genuine completion, or a diagnosed impossibility.
* **Atomic piece placement** — `pwrite` at `index × 512 KB` under
  `file_mutex`; short writes are treated as failures.
* **Streaming-friendly mode** — `sequential` ordering for in-order playback
  use cases.

### Robustness & UX
* **Malformed-input hardening** — the tracker's command parser validates every
  arity and parses integers through exception-guarded `stoll`/`stoi`; the
  client's raw-mode editor swallows every control byte and escape sequence
  without corrupting the line buffer.
* **Interactive terminal** — dynamic colored prompt (identity shown after
  login), banner, `help`, semantic coloring of tracker responses
  (SUCCESS/ERROR), full line editing (arrows, Home/End, Delete, Ctrl+A/E/U/K/W/L),
  tab completion (command names, then file paths with `~` expansion), and
  persistent history (`~/.p2p_client_history`, capped, deduplicated).
* **Leveled logging** — ERROR/WARN always visible; debug chatter compiled out
  of release builds via `P2P_LOG_LEVEL`; all logs to stderr so stdout stays
  clean when redirected. `make` (release, `-O2`, quiet) vs `make debug`
  (`-g -O0` + full logging) build into separate object trees.
* **Persisted client state** — per-user seeded-file metadata on disk is
  reloaded at login; completed files are re-registered as shares.

### Testing (what "verified" concretely means)
* **48 unit tests** (dependency-free framework, run under ASan + UBSan with
  leak detection) over hashing, protocol framing, tracker state machine, and
  download logic.
* **Integration harness** (`bash tests/integration_test.sh`) with real
  processes and ports:
  * **S1** single-seeder transfer, byte-identical result;
  * **S2** multi-peer download from two seeders at once;
  * **S3** live failover — primary killed, backup promotes and serves;
  * **S4** corrupt-source resilience — the seeder's bytes are mutated
    mid-download; bad pieces must be hash-rejected and re-fetched until the
    final file is byte-identical.

---

## 6. Design idea catalog (the "why" behind each mechanism)

| Idea | Mechanism in this codebase | Payoff |
|---|---|---|
| *Metadata ≠ data* | tracker stores hashes + peer lists only | tracker is tiny, replicable, never a bandwidth bottleneck |
| *Verify everything* | per-piece SHA-1 + final whole-file digest | corruption from any source (network, disk, malicious peer) is caught |
| *Rarest-first scheduling* | `RarestFirstSelector` over live rarity counts | swarm health; protects data closest to extinction |
| *Strategy pattern at the scheduling core* | `PieceSelector` interface + 3 implementations | algorithm comparison and per-download tuning without recompiling |
| *Failure budgets* | `MAX_PIECE_ATTEMPTS`, exhaustion blacklist, no-progress abort | every download provably terminates; one bad peer can't stall the swarm |
| *Possession-truthful bitfields* | partial-seeding bitfield from live `pieces_we_have` | leechers contribute immediately; aggregate throughput rises |
| *Cheap quorum-free failover* | probe-based election for exactly two trackers | no consensus complexity; 2-second bounded failover |
| *Command replication with sync fallback* | verbatim forwarding + queue + `FULL_STATE_SYNC` | backup converges without per-command durability requirements |
| *Framed TCP* | 4-byte length prefix everywhere | no partial-message bugs; binary payloads safe |
| *Crash-safe persistence* | append-only history file; append metadata + reload at login | client state survives restarts without a database |

---

## 7. Limitations and future work

Honest notes on the current design, useful both as caveats and as a roadmap:

1. **Passwords are stored and replicated in plaintext** (`USER <id> <password>`
   appears in the state serialization). A salted KDF (Argon2/bcrypt) and
   hashing only the digest would be the single highest-value security fix.
2. **Two-tracker assumption** — the election logic treats N = 1 and N = 2 as
   special cases; N > 2 would need a real lease/quorum design.
3. **No chunk-request pipelining** — a worker requests one piece at a time, so
   per-peer throughput is bounded by RTT. In-flight windows (like BitTorrent's
   pipeline depth of 5) would raise single-peer speed several-fold.
4. **No end-to-end encryption** — data plane and control plane are plaintext
   TCP; TLS on both planes is the natural upgrade.
5. **No NAT traversal** — peers must be mutually reachable by `ip:port`;
   UPnP/IGD or hole-punching via a relay would broaden deployability.
6. **Hash algorithm is fixed to SHA-1** — fine for integrity (not for
   adversarial collision resistance); a SHA-256 path via the same EVP API is
   a small change.
7. **Coordinator polling cadence** — the scheduler's 100 ms sleep adds a small
   scheduling latency per assignment; an event-driven handoff from worker
   completion to coordinator would shave it.

---

## 8. Build & try it

```bash
make            # release (-O2, logging compiled out)
make debug      # debug (-g -O0, full logging)
make check      # unit tests + sanitized run + integration harness

# Terminal 1: tracker (primary)
./tracker/tracker tracker_info.txt 1
# Terminal 2: tracker (backup)
./tracker/tracker tracker_info.txt 2
# Terminal 3+: clients (first arg = own listen address)
./client/client 127.0.0.1:6001 tracker_info.txt
```

Then `help` in the client for the command list, `create_user` / `login`,
`create_group`, `upload_file`, and `download_file <group> <file> <dest> [alg]`.
