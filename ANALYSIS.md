# Peer-to-Peer Distributed File Sharing System — Design & Algorithm Analysis

A BitTorrent-inspired file sharing system in C++ (Linux, POSIX sockets, OpenSSL EVP)
with a fault-tolerant tracker, parallel multi-peer downloads, pluggable piece
selection strategies, partial seeding, and end-to-end SHA-1 integrity.

```
 Repository layout
 ├── common/            protocol framing + SHA-1 hashing (shared by tracker & client)
 ├── tracker/           state machine, replication manager, client-facing server
 ├── client/            line-editor CLI, download manager, peer server, piece selectors
 └── tests/             46 unit tests (ASan/UBSan) + 4-scenario integration harness
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
| **Verified by** | 46 dependency-free unit tests under AddressSanitizer + UBSan, plus an integration harness covering transfers, multi-peer downloads, live tracker failover, and mid-download corruption. |

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
| **Client** | main raw-mode input loop (`select` on stdin + tracker socket) · one download thread per active download · one self-scheduling `peer_worker` thread per remote peer · peer-server accept loop · per-download completion/upload hooks |

The line editor's state (`current_line`, `cursor_pos`, `current_user_id`) is
only modified under `cout_mutex`, because background threads redraw the prompt
when they log. Download state is guarded by a dedicated `state_mutex`; disk
writes need no lock because `pwrite` is positional and each piece is written
by exactly one worker.

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
  │── HANDSHAKE <group> <file_name> ────▶│  (file must be known locally)
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
  // -1 = this peer has nothing to offer right now
  virtual int select_piece(const DownloadState& state,
                           const PeerState& peer) = 0;
};
```

A strategy is always asked on behalf of one peer, so it only ever returns a
piece that peer can serve. Shared state every strategy consults (all under
`state_mutex`):

```
pieces_we_have[]      acquired pieces
pieces_in_progress[]  currently being fetched by some worker
piece_rarity[]        count of active peers holding each piece
peer.bitfield[]       pieces this peer can still usefully serve
```

#### Rarest-first (default)

```
select_piece(peer):
    best ← -1
    for each piece i in peer.bitfield, not in (have ∪ in_progress):
        if best = -1 or rarity[i] < rarity[best]:
            best ← i
    return best
```

*Complexity:* O(P) per selection (P pieces, P ≤ file/512 KB).

**Why rarest-first matters** (the same reasoning as BitTorrent): the rarest
pieces are the ones closest to disappearing if their holders leave. Downloading
them first converts the swarm's most fragile knowledge into widely-replicated
data. It also naturally diversifies which pieces different leechers fetch,
increasing the number of useful partial seeders for everyone else. Rarity
counts only *currently active* peers: when a worker retires, its pieces are
subtracted again.

#### Sequential — `0, 1, 2, …`
Lowest-index servable piece first. Useful for streaming-style access (early
pieces arrive in order); worst case for swarm health since every leecher
contends for the same prefix.

#### Random — uniform over servable pieces
One pass of reservoir sampling over the peer's candidate pieces (no
allocation, `std::mt19937` seeded once per download). Good as a contention-spreading baseline and as
a fuzz-ish stress input for the scheduler.

Unknown algorithm names are rejected with a warning and fall back to
rarest-first — the CLI can never be tricked into a null strategy.

### 4.2 Download orchestration — self-scheduling workers

`start_download` opens the output file, starts one `peer_worker` thread per
peer and joins them. There is no coordinator loop: each worker schedules
itself.

```
peer_worker (per peer):
  connect → HANDSHAKE → BITFIELD; rarity[i]++ for each piece the peer has
  loop (under state_mutex):
    wait(cv) until  select_piece(self) ≠ -1          # work for this peer
                or  no piece this peer has is in flight elsewhere
    if nothing selected: exit                        # peer has nothing left
    in_progress[idx] ← true;  unlock
    REQUEST_PIECE → PIECE → verify SHA-1 → pwrite(fd, data, offset)
    lock;  in_progress[idx] ← false
      ok        → have[idx] ← true, count++
      bad hash  → peer.bitfield[idx] ← false, rarity[idx]--, strike++
      I/O error → drop this peer
    cv.notify_all()
    if 3 consecutive strikes: drop this peer
  retire: rarity[i]-- for the peer's remaining pieces; cv.notify_all()

start_download:  success ⇔ downloaded == num_pieces after all workers exit
```

Key correctness and liveness properties:

* **No polling.** A worker picks its next piece the moment the previous one
  finishes, so throughput is bounded by the network and SHA-1, not by a
  scheduler tick. Idle workers block on the condition variable.
* **Termination is progress-gated.** A worker only waits while a piece it
  could serve is in flight on another connection (that fetch may fail and
  free the piece). Otherwise it exits. When the last worker exits, the
  download is over: complete if every piece arrived, failed otherwise.
* **Bad peers cannot poison the swarm.** A dead connection ends that worker
  at once. A piece that fails verification is never requested from that peer
  again, and three consecutive bad pieces drop the peer, so other peers
  supply those pieces instead.
* **Writes are positional.** Pieces land via `pwrite(output_fd, data,
  piece_index * PIECE_SIZE)` with no file lock — a piece counts as downloaded
  only if the *entire* payload reached the disk.

### 4.3 Integrity — per-piece SHA-1 verification

`hash_buffer()` (OpenSSL EVP SHA-1) is computed over each received piece and
compared against the publisher's manifest entry. A mismatch is rejected
*before* any disk write: the piece is released for other peers, the offending
peer loses its claim to that piece, and it is dropped after three consecutive
bad pieces. Because every piece is verified before it is written and each is
written exactly once, a download that collected all pieces needs no second
whole-file pass. Only then does the downloader announce the file and (via the
tracker) promote itself from leecher to seeder.

The tracker protects the manifest itself: `upload_file` under an existing
name is accepted only if size and hashes match the registered file, so a
second seeder can never serve content that fails other leechers' checks.

The publisher pays the hashing cost once at upload time: `compute_hashes`
reads the file incrementally (constant memory, regardless of file size) and
registers `concatenated_hashes` with the tracker.

### 4.4 Partial seeding — swarm acceleration

A leecher is simultaneously a server for the pieces it already holds. Its
peer-server builds bitfields from live download state:

```cpp
// client_peer.cpp (abridged)
std::string bitfield(num_pieces, '1');               // complete seeder
if (file.download) {                                 // still downloading
  for (i = 0; i < num_pieces; ++i)                   // true possession
    if (!file.download->pieces_we_have[i]) bitfield[i] = '0';
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

**Role election (`ReplicationManager::run_backup_mode`):**

Only a primary accepts connections on the replication port, and it speaks
first (the full state). Everyone else leaves connections in the listen
backlog, so a caller hears silence.

```
if N == 1:  become PRIMARY immediately            # no peer to elect with
loop:
    connect to the OTHER tracker's replication port (2 s timeout)
    cannot connect            → PRIMARY   (nobody else is running)
    first message arrives     → BACKUP    (follow that primary)
    connected but silent 2 s:
        I was following it    → PRIMARY   (it hung)
        my id is lower        → PRIMARY   (both undecided: tie-break)
        otherwise             → retry in 0.5 s
```

A successful TCP connect alone proves nothing about the other tracker's role,
which is why the decision waits for the first message. Two trackers started
at the same instant therefore resolve to exactly one primary within about two
seconds, instead of each mistaking the other for a primary.

**State synchronization:**

* When a backup connects, the primary sends `FULL_STATE_SYNC` — the entire
  `TrackerState` serialized (`USER`, `GROUP`, `FILE …` records). The snapshot
  and the switch to live forwarding happen under one lock, so no command can
  fall between them. A reconnecting backup replaces the previous connection.
* While connected, every mutating command is forwarded verbatim
  (`forward_command`) and replayed by the backup via
  `process_replicated_command`. Nothing is queued while no backup is
  connected: the next full sync covers it.
* Sessions (logins, peer addresses) are deliberately not replicated.
* The primary sends `HEARTBEAT` every 2 s on the replication socket; a failed
  send drops the link until the backup reconnects.

**Failover (backup side):** if the connection to the primary drops, or no
message arrives for 6 s (three heartbeats), the backup re-runs the election
loop above. Only if the primary is unreachable or stays silent does it
promote itself; it then starts with **no client sessions** (login state is
per-connection and cannot be safely preserved) and serves new connections
from its replicated state. A killed primary is replaced within a second; a
hung one within about eight.

Client side, the failover is mostly transparent: clients hold both tracker
addresses and retry down the list at startup (`Trying next...`), and the
download/leech state is restored by re-issuing `upload_file`/`stop_leeching`
as needed.

### 4.6 Concurrency model summary

| Shared state | Protection |
|---|---|
| Download piece bookkeeping (`pieces_*`, `piece_rarity`) | `state_mutex` + condition variable |
| Output file writes | lock-free `pwrite` at fixed offsets, one writer per piece |
| Console I/O | `cout_mutex` + redraw-above-prompt line discipline |
| Login state | `std::atomic<bool>` |
| Tracker state (users/groups/files) | `TrackerState` internal mutex; backup socket guarded by a separate replication mutex |
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
* **Bad-peer handling** — a failed piece is never requested from the same
  peer again, three bad pieces in a row drop the peer (4.3), and workers exit
  when nothing is servable (4.2), giving every download a guaranteed
  termination path: completion or a diagnosed impossibility.
* **Atomic piece placement** — `pwrite` at `index × 512 KB`; short writes are
  treated as failures.
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
  clean when redirected. `make` (debug, full logging) vs `make release`
  (`-O2`, quiet) build into separate object trees.
* **Persisted client state** — per-user seeded-file metadata on disk is
  reloaded at login; completed files are re-registered as shares.

### Testing (what "verified" concretely means)
* **46 unit tests** (dependency-free framework, run under ASan + UBSan with
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
| *Verify everything* | per-piece SHA-1 before every write; tracker rejects mismatched manifests | corruption from any source (network, disk, malicious peer) is caught |
| *Rarest-first scheduling* | `RarestFirstSelector` over live rarity counts | swarm health; protects data closest to extinction |
| *Strategy pattern at the scheduling core* | `PieceSelector` interface + 3 implementations | algorithm comparison and per-download tuning without recompiling |
| *Failure budgets* | per-peer piece blacklist, three-strike peer drop, exit when nothing is servable | every download provably terminates; one bad peer can't stall the swarm |
| *Possession-truthful bitfields* | partial-seeding bitfield from live `pieces_we_have` | leechers contribute immediately; aggregate throughput rises |
| *Cheap quorum-free failover* | first-message election with id tie-break for exactly two trackers | no consensus complexity; bounded failover |
| *Command replication with sync fallback* | verbatim forwarding + `FULL_STATE_SYNC` on every (re)connect | backup converges without per-command durability requirements |
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
7. **Bitfields are a handshake-time snapshot** — a leecher's bitfield is read
   once, so pieces it acquires later are not requested from it by peers that
   connected earlier. `HAVE` announcements would fix this.
8. **A resumed hung primary is not demoted** — if a primary stalls long enough
   for the backup to take over and then resumes, both believe they are
   primary. A lease or epoch number would close this.

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
