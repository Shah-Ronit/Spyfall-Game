# Spyfall — Multiplayer Network Game
### C++ · TCP Sockets · POSIX · Rooms + Private DM Edition

A terminal-based multiplayer **Spyfall** clone built with raw POSIX sockets and `std::thread`. Designed as a Computer Networks course project demonstrating client-server architecture, concurrent connection handling, scoped broadcasting across isolated game rooms, and point-to-point private messaging.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Architecture](#2-architecture)
3. [File Structure](#3-file-structure)
4. [Build & Run](#4-build--run)
5. [How to Play](#5-how-to-play)
6. [Full Command Reference](#6-full-command-reference)
7. [Protocol Specification](#7-protocol-specification)
8. [Feature Deep-Dive](#8-feature-deep-dive)
9. [Networking Concepts Demonstrated](#9-networking-concepts-demonstrated)
10. [Known Limitations](#10-known-limitations)

---

## 1. Project Overview

Spyfall is a social deduction game. At the start of each round:

- Every player except one is told the **location** (e.g. *Hospital*, *Airport*).
- One player — the **spy** — is told nothing and must figure out the location by listening to others' answers.
- Civilians ask each other questions to expose the spy without giving the location away.
- After several turns, players vote for who they think the spy is.
- **Civilians win** if they correctly identify the spy. **The spy wins** if they survive the vote or correctly guess the location.

This implementation adds:

- **Multiple isolated game rooms** — players create and join named rooms; each room runs its own independent game.
- **Private DM chat** — any player can send a private message to any other player at any time, regardless of room.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────┐
│                        SERVER                           │
│                                                         │
│   accept() loop (main thread)                           │
│       │                                                 │
│       ├── handleClient(fd)  [thread per client]         │
│       ├── handleClient(fd)  [thread per client]         │
│       └── handleClient(fd)  [thread per client]         │
│                                                         │
│   autoStartWatcher()  [background thread]               │
│       polls all rooms every 1s for MIN_PLAYERS          │
│                                                         │
│   Shared State (protected by g_mutex)                   │
│       g_allPlayers : vector<Player*>                    │
│       g_rooms      : map<string, Room*>                 │
│           Room "alpha" → { players, state, location }   │
│           Room "beta"  → { players, state, location }   │
└─────────────────────────────────────────────────────────┘
          ▲ TCP ▼                    ▲ TCP ▼
   ┌──────────────┐          ┌──────────────┐
   │   CLIENT A   │          │   CLIENT B   │
   │              │          │              │
   │ recv thread  │          │ recv thread  │
   │ main thread  │          │ main thread  │
   │ (stdin read) │          │ (stdin read) │
   └──────────────┘          └──────────────┘
```

### Key design choices

| Choice | Rationale |
|---|---|
| One `std::thread` per client | Simple to reason about; each thread blocks on `recv()` independently |
| Single `std::mutex` (`g_mutex`) | Protects all shared state; held briefly then released |
| `Room` struct owns its player list | Prevents accidental cross-room broadcasts |
| `Player::room` pointer | O(1) room lookup per command — no searching |
| Two broadcast variants | `broadcastLocked()` (inside lock) vs `startRoomGame()` (takes lock) prevents deadlocks |
| DM uses direct `send()` | Point-to-point — never touches a broadcast loop |

---

## 3. File Structure

```
spyfall/
├── server.cpp      — game server (compile separately)
├── client.cpp      — terminal client (compile separately)
└── README.md       — this file
```

---

## 4. Build & Run

### Requirements

- Linux (POSIX sockets)
- GCC or Clang with C++11 or later
- `pthread` library

### Compile

```bash
g++ server.cpp -o server -pthread
g++ client.cpp -o client -pthread
```

### Run the server

```bash
./server           # listens on default port 12345
./server 9000      # listens on port 9000
```

### Run clients (each in its own terminal)

```bash
./client                        # connects to 127.0.0.1:12345
./client 192.168.1.10           # custom server IP
./client 192.168.1.10 9000      # custom IP and port
```

### Quick local test with 4 players (4 terminals)

```bash
# Terminal 1
./client
> JOIN Alice
> CREATE_ROOM game1
> JOIN_ROOM game1

# Terminal 2
./client
> JOIN Bob
> JOIN_ROOM game1

# Terminal 3
./client
> JOIN Carol
> JOIN_ROOM game1

# Terminal 4
./client
> JOIN Dave
> JOIN_ROOM game1
# Game auto-starts in 3 seconds
```

---

## 5. How to Play

### Step 1 — Register

```
> JOIN <your_name>
```

You are now in the **global lobby**. Names must be unique across the entire server.

### Step 2 — Find or create a room

```
> LIST_ROOMS                  # see all rooms and their state
> CREATE_ROOM mygame          # create a new room called "mygame"
> JOIN_ROOM mygame            # enter an existing room
```

### Step 3 — Wait for players / start game

The game auto-starts **3 seconds** after the 4th player joins a room. You can also trigger it manually:

```
> START
```

### Step 4 — Receive your role

- **Spy** → you see `ROLE SPY`. You know nothing about the location.
- **Civilian** → you see `ROLE LOCATION <place>`. You know the location.

### Step 5 — Take turns

The server announces whose turn it is with `TURN <name>`. Only that player can send a `MSG`.

```
> MSG I think this place has a lot of security cameras.
```

Everyone in the room sees: `💬 Alice: I think this place has a lot of security cameras.`

### Step 6 — Vote

After all players have spoken twice (`TURNS_PER_ROUND = 2`), voting opens automatically.

```
> VOTE Bob
```

The player with the most votes is revealed. If it's the spy, civilians win. Otherwise, the spy wins.

### Step 7 — Private messaging (any time)

```
> DM Bob Hey, are you sure Carol isn't the spy?
```

Only Bob receives this. It appears with a 🔒 icon and dark background to distinguish it from room messages. Works from lobby, inside a room, or mid-game.

### Step 8 — After the game

```
> RESTART          # replay in the same room (resets all roles and votes)
> LEAVE_ROOM       # go back to lobby
> QUIT             # disconnect
```

---

## 6. Full Command Reference

### Lobby commands

| Command | Description |
|---|---|
| `JOIN <name>` | Register your player name. Must be done first. |
| `LIST_ROOMS` | Show all rooms, their game state, and player count. |
| `CREATE_ROOM <id>` | Create a new room with the given ID. |
| `JOIN_ROOM <id>` | Enter an existing room. Rejected if game is in progress. |

### In-room commands

| Command | Description |
|---|---|
| `START` | Manually start the game (requires ≥ 4 players). |
| `MSG <text>` | Speak during the game. Only works on your turn. |
| `VOTE <player>` | Vote for who you think the spy is. Only works during voting phase. |
| `RESTART` | Start a new game in the same room after one finishes. |
| `LEAVE_ROOM` | Exit the room and return to the global lobby. |

### Global commands (work anywhere)

| Command | Description |
|---|---|
| `DM <player> <text>` | Send a private message to any online player. |
| `STATUS` | Show your current room, game state, and player list. |
| `QUIT` | Disconnect from the server. |
| `help` | Print the command reference (client-side only). |

---

## 7. Protocol Specification

All messages are plain text, newline-delimited (`\n`). No JSON, no binary framing.

### Client → Server

```
JOIN <name>
LIST_ROOMS
CREATE_ROOM <room_id>
JOIN_ROOM <room_id>
LEAVE_ROOM
START
MSG <message text>
VOTE <player_name>
RESTART
DM <target_name> <message text>
STATUS
QUIT
```

### Server → Client

```
WELCOME <text>                    — greeting on connect
JOINED <text>                     — JOIN acknowledged
ROOM_LIST <id>[STATE,Np] ...      — room listing
ROOM_CREATED <id>                 — room creation confirmed
ROOM_JOINED <id>                  — you entered a room
ROOM_LEFT                         — you left a room
ROOM_PLAYERS <n1> <n2> ...        — current room occupants
INFO <text>                       — general notification
ROLE SPY                          — you are the spy this round
ROLE LOCATION <place>             — you are a civilian; location is <place>
GAME_START <text>                 — round has started
TURN <player_name>                — it is this player's turn to speak
MSG <player>: <text>              — in-room broadcast message
VOTING_START <text>               — voting phase has begun
VOTE_CAST <text>                  — a player cast a vote
VOTE_STATUS <n>/<total> votes     — running vote tally
RESULT <text>                     — vote outcome
REVEAL <text>                     — spy identity disclosed
DM_FROM <sender> <text>           — private message received
DM_SENT <target> (delivered)      — your DM was delivered
STATUS <text>                     — current state info
ERROR <text>                      — command rejected with reason
BYE <text>                        — server confirms disconnect
```

---

## 8. Feature Deep-Dive

### Multiple game rooms

Each room is a `Room` struct containing its own player list, game state machine, location, and turn counter. The server holds all rooms in a `std::map<std::string, Room*>` keyed by room ID.

When a player sends any in-game command (`MSG`, `VOTE`, `RESTART`), the handler reads `self->room` to get the relevant room and calls `room->broadcast()`, which iterates only that room's player list. Messages are **physically incapable** of crossing room boundaries.

Rooms persist for the life of the server. A finished room can be restarted with `RESTART` — players don't need to reconnect or re-join.

### Private DM chat

The `DM` command does a global name lookup across `g_allPlayers` (not scoped to any room), then calls `send()` on exactly two file descriptors: the recipient's socket and the sender's socket for the confirmation. No broadcast, no loop.

This means:
- A player in room "alpha" can DM a player in room "beta" mid-game.
- A player in the lobby can DM a player who is in the middle of a vote.
- The spy can coordinate privately with nobody — or try to trick civilians.

DMs are rendered with a 🔒 icon and a distinct dark background on the client so they are visually unmistakable from room messages even when both arrive in rapid succession.

### Thread safety

Every access to `g_allPlayers`, `g_rooms`, or any `Room` or `Player` field goes through `std::lock_guard<std::mutex> lk(g_mutex)`. The lock is held for the duration of each command handler, then released before any blocking I/O or sleep. The `send()` calls themselves happen inside the lock — they are fast and non-blocking in practice at this scale.

The one deliberate lock-free operation is `std::atomic<bool> g_serverRunning`, which is written only on shutdown and read by the auto-start watcher loop without needing the mutex.

### Auto-start watcher

A background thread (`autoStartWatcher`) wakes up every second, scans all rooms for the `WAITING` state with ≥ `MIN_PLAYERS` connected players, sleeps 3 more seconds (giving players time to see the notification), then calls `startRoomGame()` if the condition still holds. This gives players a countdown window and prevents instant starts when players join simultaneously.

---

## 9. Networking Concepts Demonstrated

| Concept | Where |
|---|---|
| TCP socket creation (`socket`, `bind`, `listen`, `accept`) | `main()` in server.cpp |
| Client connection (`socket`, `connect`) | `main()` in client.cpp |
| One thread per connection | `std::thread(handleClient, cfd, ...).detach()` |
| Shared state with mutex | `g_mutex` protecting `g_allPlayers` and `g_rooms` |
| Scoped broadcast | `Room::broadcast()` iterates only room's player list |
| Point-to-point routing | `DM` handler calls `send()` on two specific fds |
| Framing / message delimiting | All messages end with `\n`; clients accumulate partial reads |
| Graceful disconnect | `recv() ≤ 0` triggers cleanup; `shutdown(SHUT_RDWR)` unblocks receiver |
| Client duplex I/O | Separate `std::thread` for `recv`, main thread for `stdin` |

---

## 10. Known Limitations

- **No encryption** — all messages including role assignments travel in plaintext. This is intentional for a course demo; do not deploy on untrusted networks.
- **No reconnection** — if a client disconnects mid-game their player slot is freed. A reconnecting player must `JOIN` with a new name.
- **Room never deleted** — rooms accumulate for the server's lifetime. For long-running servers, add a cleanup pass for empty finished rooms.
- **One vote per player, no abstain** — every connected player must vote before results are tallied.
- **No spectator mode** — players who join a room while a game is in progress must wait for it to finish.
- **Global name uniqueness** — two players in different rooms cannot share a name, which is required for DM routing to be unambiguous.
