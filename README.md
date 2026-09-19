# Peer-to-Peer Distributed File Sharing System

This project is a peer-to-peer (P2P) distributed file sharing system implemented in C++ for the Advanced Operating Systems course (Monsoon 2025). It allows users to form groups, share files within those groups, and download files in parallel from multiple peers to accelerate transfer speeds. The system features a fault-tolerant, replicated tracker system for high availability and an intelligent client designed for efficient and cooperative network participation.

## Table of Contents
1.  [Architectural Overview](#1-architectural-overview)
2.  [Features](#2-features)
3.  [Compilation and Execution](#3-compilation-and-execution)
4.  [System Design and Implementation](#4-system-design-and-implementation)
5.  [Key Algorithms](#5-key-algorithms)
6.  [Network Protocol](#6-network-protocol)
7.  [Testing](#7-testing)
8.  [Assumptions and Limitations](#8-assumptions-and-limitations)
9.  [Manual Test Procedures](#9-manual-test-procedures)

---

## 1. Architectural Overview

The system operates on a **hybrid P2P model**. It uses centralized trackers for metadata management while facilitating direct client-to-client connections for the actual file data transfers. This design leverages the scalability of P2P networks while maintaining the organizational benefits of a client-server architecture for tasks like user authentication and peer discovery.

The system is composed of two primary components:
* **Tracker System:** A fault-tolerant cluster of two tracker servers that manage user accounts, group memberships, and file metadata. They maintain a synchronized state to ensure high availability, allowing the system to function as long as at least one tracker is online.
* **Client System:** A multi-threaded application where each client acts as both a downloader (**leecher**) and an uploader (**seeder**). It handles user commands, manages local file state, and communicates directly with other peers to download or upload file pieces in parallel.

---

## 2. Features

* **User and Group Management:** Users can create accounts, log in, create groups, join groups (with owner approval), and list available groups.
* **Fault-Tolerant Trackers:** Two trackers operate in a Primary-Backup model. If the Primary fails, the Backup is automatically promoted, ensuring service continuity.
* **Multi-Peer Parallel Downloads:** Files are downloaded from multiple peers simultaneously, significantly increasing download speed.
* **Intelligent Piece Selection:** Implements multiple piece selection strategies, with **Rarest-First** as the default to improve swarm health and download resilience.
* **Partial Seeding:** A client can begin uploading pieces it has already downloaded to other peers, even before its own download is 100% complete.
* **File Integrity:** All files and file pieces are verified using **SHA1 hashes** to ensure data is not corrupted during transfer.
* **Robust Concurrency:** The system is heavily multi-threaded and uses thread-safe data structures and synchronization primitives (`std::mutex`, `std::condition_variable`) to handle concurrent operations safely and efficiently.
* **Tested:** A dependency-free unit test suite (48 tests, runs under AddressSanitizer + UBSan) plus an end-to-end integration harness covering real transfers, tracker failover, and corrupt-peer resilience. See [Testing](#9-testing).

---

## 3. Compilation and Execution

### Compilation
The project uses a standard Makefile with two build modes:

```bash
$ make            # release build: -O2, verbose logging compiled out
$ make debug      # debug build: -g -O0 + P2P_DEBUG, full per-command logging
```

Both modes produce `./tracker/tracker` and `./client/client`. They use separate
object directories (`build/` vs `build/debug/`), so you can switch between them
without a clean.

**Logging:** all log output is leveled and colored. `ERROR`/`WARN`/important
events always print; per-command `DEBUG` chatter is compiled out of release
builds (via the `P2P_LOG_LEVEL` macro) and only enabled by `make debug`.
Log messages go to stderr, so normal command output on stdout stays clean
when redirecting.

### Execution
The system requires at least one tracker and one client to be running. For a fault-tolerant setup, run both trackers.

**Terminal 1: Start Trackers**
```bash
# Start the first tracker (will become Primary)
$ ./tracker/tracker tracker_info.txt 1

# In a new terminal, start the second tracker (will become Backup)
$ ./tracker/tracker tracker_info.txt 2
```

**Terminal 2 (and others): Start Clients**
```bash
# Start a client: first arg is this peer's own listen address (IP:PORT),
# second is the tracker info file.
$ ./client/client 127.0.0.1:6001 tracker_info.txt
```
The client connects to the Primary tracker and shows a colored `alice@p2p >>`
prompt (dim `anon@p2p` until you log in), with full line editing:

* `←`/`→` move the cursor, `Home`/`End` (or `Ctrl+A`/`Ctrl+E`) jump to the
  start/end, `Ctrl+U`/`Ctrl+K`/`Ctrl+W` clear line/to end/previous word.
* `↑`/`↓` walk through command history (up to 200 entries).
* `Tab` completes commands for the first word (e.g. `li<Tab>` → `list_`,
  then lists `list_groups` / `list_requests` / `list_files`) and file paths
  for later words (e.g. `upload_file g1 ./te<Tab>` → `./testdata.txt`),
  inserting the longest common prefix and listing ambiguous matches.
* Command history persists across runs in
  `~/.p2p_client_history` (deduplicated, capped at 200 entries), so `↑`
  still recalls commands from previous sessions.
* `Ctrl+L` (or typing `clear`) clears the screen; type `help` for the command list.

Malformed commands, unknown commands, and tracker errors are reported in red
without ever crashing the client or corrupting the input line.

---

## 4. System Design and Implementation

### Tracker System

#### Synchronization Model: Primary-Backup
To achieve fault tolerance, a Primary-Backup model was implemented:
1.  **Role Election:** On startup, each tracker attempts to connect to the other on a dedicated replication port. The first one to start fails to connect and assumes the **Primary** role. The second tracker connects successfully and assumes the **Backup** role.
2.  **State Replication:** The Primary handles all client interactions. For any state-changing command (e.g., `create_user`), it forwards the command string to the Backup, which applies the same change to its in-memory state.
3.  **Failover:** If the Backup loses its connection to the Primary, it promotes itself to Primary and begins accepting client connections. It also invalidates all user sessions, forcing clients to re-login to ensure a consistent state.

#### Data Structures
* `std::unordered_map` is used for primary data containers (`users`, `groups`, `files`) for its efficient average-case O(1) time complexity for lookups.
* `std::set` is used for group members to ensure deterministic ordering. This is critical for failover scenarios, as it guarantees that both Primary and Backup will elect the same new group owner if the current one leaves.

### Client System

#### Threading Model: Producer-Consumer
The client download process is built on a multi-threaded producer-consumer pattern:
* **Download Manager (Producer):** A dedicated thread that orchestrates the download. It selects pieces to download using the chosen strategy and assigns them as "work" to idle Peer Worker threads.
* **Peer Workers (Consumers):** One thread per peer connection. Each worker waits on a condition variable. When assigned a piece by the manager, it wakes up, downloads the piece, verifies its hash, and writes it to disk. Upon completion, it signals the manager that it is idle and ready for more work.

#### State Management & Seeding
* The client creates a hidden `.p2p_client/` directory in the user's home folder to store persistent metadata.
* For each shared file, a `.meta` file is created containing the absolute path to the original file.
* **Seeding is path-dependent.** The client reads directly from the original file path to serve pieces to other peers. If the user moves, renames, or deletes a file after sharing it, seeding for that file will fail.

---

## 5. Key Algorithms

### Piece Selection Strategy
The client implements the **Strategy design pattern** for piece selection, allowing the user to choose an algorithm at runtime.
```bash
download_file <group_id> <file_name> <dest_path> [rarest|sequential|random]
```

#### Rarest-First Algorithm
The default and most effective strategy is Rarest-First.
1.  **Data Collection:** As the client connects to peers, it builds a real-time census of which pieces are available on which peers. This is stored in a `piece_rarity` vector where each index holds the count of peers who have that piece.
2.  **Selection Logic:** When choosing a piece to download, the manager selects the piece that it doesn't have, is not currently in progress, and has the **lowest count** in the `piece_rarity` vector. This prioritizes pieces that are in danger of disappearing from the swarm.

### Synergistic Swarm Efficiency
The system's true power comes from the synergy between two features:
1.  **Partial Seeding:** A client can begin uploading pieces it has already acquired even while its own download is in progress. This is possible because the seeder logic can construct an accurate `BITFIELD` from its in-progress `DownloadState`.
2.  **Rarest-First Synergy:** The Rarest-First algorithm causes a client to acquire the most valuable (rarest) pieces first. Partial seeding then allows the client to immediately start sharing these high-value pieces with the rest of the swarm. This transforms a new downloader into a critical distributor of rare data, improving the health and speed of the entire network.

---

## 6. Network Protocol

All communication occurs over TCP sockets using a custom, length-prefixed protocol to ensure reliable message framing. A 4-byte header indicates the length of the upcoming message payload.

### Client-Tracker Protocol
* `create_user <user_id> <password>`
* `login <user_id> <password> <peer_port>`
* `create_group <group_id>`
* `join_group <group_id>`
* `list_files <group_id>`
* `upload_file <group_id> <file_name> <size> <hashes>`
* `download_file <group_id> <file_name>`
* **Response Format:** `SUCCESS: <message>` or `ERROR: <message>`

### Client-Peer Protocol
* `HANDSHAKE <file_name>`
* `BITFIELD <011010...>`
* `REQUEST_PIECE <piece_index>`
* `PIECE <piece_index> <binary_data>`

---

## 7. Testing

The project ships with a three-layer test rig, all wired into the Makefile:

```bash
make test            # unit tests (fast, no sanitizers)
make test_sanitize   # same suite under ASan + UBSan with leak detection
make integration     # end-to-end: real trackers, real clients, real transfers
make check           # all three, in order
```

### Unit tests (`tests/test_*.cpp`)
A minimal dependency-free framework (`tests/test_assert.h`) with `TEST(name)`,
`TEST_ASSERT`, `TEST_ASSERT_EQ`, and `TEST_ASSERT_THROWS`. Each binary links
only the objects it exercises, so failures isolate the offending module:

| Suite | Covers |
|---|---|
| `test_protocol` | Length-prefixed framing: round-trips, binary payloads with NUL bytes, >1 MB messages through the partial send/recv loops, message coalescing, `EINTR` retries, oversized-prefix rejection, send/recv on closed sockets |
| `test_hashing` | SHA-1 known-answer vectors, piece boundaries (exact multiples, partial final piece, empty file), missing-file error |
| `test_tracker_state` | User/group/file semantics, owner promotion, leecher lifecycle, replication replay equivalence (backup state must match primary), malformed/controlled-command tolerance, `FULL_STATE_SYNC` rebuild |
| `test_download_logic` | Piece-selection strategies: rarest-first ordering, availability filtering, exhausted-piece skipping, sequential and random validity, empty-state handling |

### Integration tests (`tests/integration_test.sh`)
Boots the real topology (2 trackers + clients driven through stdin FIFOs) and
verifies four scenarios:

1. **S1** — single-seeder transfer; the downloaded file must be byte-identical (`cmp`).
2. **S2** — multi-peer download from two seeders simultaneously.
3. **S3** — tracker failover: the primary is killed; the backup must promote and serve a brand-new client.
4. **S4** — corrupt-source resilience: the seeder's on-disk bytes are flipped mid-download; per-piece hash verification must reject the bad data (the completed download is compared against a pristine copy).

The harness is hang-proof by construction: FIFO writes are timeout-guarded
against dead readers, port probes are bounded, and it cleans up all spawned
processes on exit. Run it with `--keep` to preserve logs under
`$TMPDIR/p2p_itest/logs` for debugging.

> **Port plan:** trackers listen on 5101/5102, replication uses 6101/6102
> (tracker port + 1000), and test clients bind 7101-7106.

## 8. Assumptions and Limitations

### Assumptions
* The network consists of exactly two trackers as defined in `tracker_info.txt`.
* The network is reasonably reliable; the system relies on TCP for error correction.
* All users are trusted not to maliciously alter the client or protocol.

### Limitations
* **Tracker State is Ephemeral:** As per the design, if both trackers shut down simultaneously, all user, group, and file metadata is lost. The system is highly available but not persistent on the tracker side.
* **Seeding is Path-Dependent:** As hardlinks were removed for simplicity, seeding relies on the original file remaining at its absolute path. If the user moves, renames, or deletes the file, seeding will break.
* **Peer Addresses Are Session State:** Peer addresses are not replicated to the backup; on failover all sessions are invalidated and clients must re-login (which re-publishes their addresses to the new primary).

---

## 9. Manual Test Procedures

A typical manual scenario can be conducted as follows:
1.  **Generate Test Files:** Use the `dd` command to create files of various sizes (e.g., 10MB, 500MB).
2.  **Start System:** Launch both trackers and at least two clients in separate terminals.
3.  **Create Users & Groups:**
    * On Client A, run:
        ```bash
        >> create_user userA passA
        >> login userA passA
        >> create_group testgroup
        ```
    * On Client B, run:
        ```bash
        >> create_user userB passB
        >> login userB passB
        >> join_group testgroup
        ```
    * On Client A, accept the request:
        ```bash
        >> list_requests testgroup
        >> accept_request testgroup userB
        ```
4.  **Upload a File:** On Client A, share a file with the group.
    ```bash
    >> upload_file testgroup /path/to/your/testfile_500MB.dat
    ```
5.  **Download the File:** On Client B, download the file.
    ```bash
    >> list_files testgroup
    >> download_file testgroup testfile_500MB.dat ./downloads/ [rarest|sequential|random]
    ```