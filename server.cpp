

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace std;



static const int    DEFAULT_PORT    = 12345;
static const int    BACKLOG         = 16;
static const int    MIN_PLAYERS     = 4;
static const int    TURNS_PER_ROUND = 2;
static const size_t BUFFER_SIZE     = 2048;

static const vector<string> LOCATIONS = {
    "Airport", "Hospital", "School", "Bank",
    "Casino",  "Museum",   "Beach",  "SpaceStation",
    "Circus",  "Embassy",  "Submarine", "PolarStation"
};

struct Room;


struct Player {
    int    fd;
    string name;
    string role;
    string vote;
    bool   connected;
    Room*  room;

    Player(int fd_, const string& n)
        : fd(fd_), name(n), role(""), vote(""), connected(true), room(nullptr) {}
};


enum class GameState { WAITING, PLAYING, VOTING, FINISHED };


struct Room {
    string          id;
    vector<Player*> players;
    GameState       state;
    string          location;
    int             currentTurn;
    int             turnCount;

    explicit Room(const string& name)
        : id(name), state(GameState::WAITING), currentTurn(0), turnCount(0) {}

    static bool sendLine(int fd, const string& msg) {
        string line = msg + "\n";
        return ::send(fd, line.c_str(), line.size(), 0) > 0;
    }

    void broadcast(const string& msg) const {
        for (auto* p : players)
            if (p->connected) sendLine(p->fd, msg);
    }

    int connectedCount() const {
        int n = 0;
        for (auto* p : players) if (p->connected) n++;
        return n;
    }

    Player* findByName(const string& name) const {
        for (auto* p : players) if (p->name == name) return p;
        return nullptr;
    }

    void announcePlayerList() const {
        string msg = "ROOM_PLAYERS";
        for (auto* p : players) if (p->connected) msg += " " + p->name;
        broadcast(msg);
    }

    string stateStr() const {
        switch (state) {
            case GameState::WAITING:  return "WAITING";
            case GameState::PLAYING:  return "PLAYING";
            case GameState::VOTING:   return "VOTING";
            case GameState::FINISHED: return "FINISHED";
        }
        return "UNKNOWN";
    }
};


mutex              g_mutex;
vector<Player*>    g_allPlayers;
map<string, Room*> g_rooms;
atomic<bool>       g_serverRunning(true);


static bool sendLine(int fd, const string& msg)
{
    string line = msg + "\n";
    return ::send(fd, line.c_str(), line.size(), 0) > 0;
}


static Player* findPlayerByName(const string& name)
{
    for (auto* p : g_allPlayers)
        if (p->connected && p->name == name) return p;
    return nullptr;
}


static void sendRoomListLocked(int fd)
{
    if (g_rooms.empty()) {
        sendLine(fd, "ROOM_LIST (empty - use CREATE_ROOM <name> to create one)");
        return;
    }
    string msg = "ROOM_LIST";
    for (auto& kv : g_rooms) {
        Room* r = kv.second;
        msg += " " + r->id + "[" + r->stateStr() + "," +
               to_string(r->connectedCount()) + "p]";
    }
    sendLine(fd, msg);
}


static void advanceTurnLocked(Room* r)
{
    vector<Player*> active;
    for (auto* p : r->players) if (p->connected) active.push_back(p);
    if (active.empty()) return;

    r->turnCount++;
    r->currentTurn = (r->currentTurn + 1) % (int)active.size();

    if (r->turnCount >= (int)active.size() * TURNS_PER_ROUND) {
        r->state = GameState::VOTING;
        r->broadcast("VOTING_START Time to vote! Send: VOTE <player_name>");
        r->broadcast("INFO Vote for who you think the spy is.");
        cout << "[" << r->id << "] Voting opened." << endl;
    } else {
        string next = active[r->currentTurn]->name;
        r->broadcast("TURN " + next);
        r->broadcast("INFO It is " + next + "'s turn to speak.");
    }
}

static void resolveVotesLocked(Room* r)
{
    map<string, int> tally;
    for (auto* p : r->players)
        if (p->connected && !p->vote.empty())
            tally[p->vote]++;

    if (tally.empty()) {
        r->broadcast("RESULT No votes cast. Game ends in a draw.");
        r->state = GameState::FINISHED;
        return;
    }

    string topName;
    int topVotes = 0;
    for (auto& kv : tally)
        if (kv.second > topVotes) { topVotes = kv.second; topName = kv.first; }

    int tieCount = 0;
    for (auto& kv : tally) if (kv.second == topVotes) tieCount++;

    Player* spy = nullptr;
    for (auto* p : r->players)
        if (p->connected && p->role == "SPY") { spy = p; break; }
    string spyName = spy ? spy->name : "Unknown";

    r->broadcast("RESULT Most voted: " + topName +
                 " (" + to_string(topVotes) + " vote(s))");
    r->broadcast("REVEAL The spy was: " + spyName);

    if      (tieCount > 1)        r->broadcast("RESULT TIE - spy survives. SPY WINS!");
    else if (topName == spyName)  r->broadcast("RESULT Correct! CIVILIANS WIN!");
    else                          r->broadcast("RESULT Wrong! " + spyName + " was the spy. SPY WINS!");

    r->broadcast("INFO Type RESTART to play again, or LEAVE_ROOM to exit.");
    r->state = GameState::FINISHED;
    cout << "[" << r->id << "] Game over. Spy=" << spyName << endl;
}

static void startRoomGame(Room* r)
{
    lock_guard<mutex> lock(g_mutex);

    if (r->state == GameState::PLAYING || r->state == GameState::VOTING) return;

    vector<Player*> active;
    for (auto* p : r->players) {
        if (p->connected) { p->role = ""; p->vote = ""; active.push_back(p); }
    }

    if ((int)active.size() < MIN_PLAYERS) {
        r->broadcast("ERROR Not enough players (need " + to_string(MIN_PLAYERS) +
                     ", have " + to_string((int)active.size()) + ").");
        return;
    }

    r->location = LOCATIONS[rand() % LOCATIONS.size()];

    int spyIdx = rand() % (int)active.size();
    for (int i = 0; i < (int)active.size(); i++)
        active[i]->role = (i == spyIdx) ? "SPY" : "CIVILIAN";

    for (auto* p : active) {
        if (p->role == "SPY") {
            Room::sendLine(p->fd, "ROLE SPY");
            Room::sendLine(p->fd, "INFO You are the SPY! Listen carefully to guess the location.");
        } else {
            Room::sendLine(p->fd, "ROLE LOCATION " + r->location);
            Room::sendLine(p->fd, "INFO You are a CIVILIAN. Location: " + r->location + ". Find the spy without revealing the location.");
        }
    }

    r->state       = GameState::PLAYING;
    r->currentTurn = 0;
    r->turnCount   = 0;

    r->broadcast("GAME_START Game started in room [" + r->id + "]!");
    r->broadcast("INFO " + to_string(active.size()) + " players. The spy is among you...");
    r->broadcast("TURN " + active[0]->name);
    r->broadcast("INFO It is " + active[0]->name + "'s turn to speak.");

    cout << "[" << r->id << "] Game started. Location=" << r->location
         << "  Spy=" << active[spyIdx]->name << endl;
}


static void autoStartWatcher()
{
    while (g_serverRunning) {
        this_thread::sleep_for(chrono::seconds(1));

        vector<Room*> candidates;
        {
            lock_guard<mutex> lock(g_mutex);
            for (auto& kv : g_rooms)
                if (kv.second->state == GameState::WAITING &&
                    kv.second->connectedCount() >= MIN_PLAYERS)
                    candidates.push_back(kv.second);
        }

        if (!candidates.empty()) {
            this_thread::sleep_for(chrono::seconds(3));
            for (Room* r : candidates) {
                bool go = false;
                {
                    lock_guard<mutex> lock(g_mutex);
                    go = (r->state == GameState::WAITING &&
                          r->connectedCount() >= MIN_PLAYERS);
                }
                if (go) startRoomGame(r);
            }
        }
    }
}

static void handleClient(int clientFd, string addr)
{
    char    buf[BUFFER_SIZE];
    string  partial;
    Player* self = nullptr;

    cout << "[SERVER] New connection: " << addr << "  fd=" << clientFd << endl;

    sendLine(clientFd, "WELCOME Spyfall Server (Rooms + DM Edition)");
    sendLine(clientFd, "INFO Commands: JOIN <name> | LIST_ROOMS | CREATE_ROOM <id> | JOIN_ROOM <id> | DM <player> <text> | QUIT");

    while (g_serverRunning) {
        memset(buf, 0, sizeof(buf));
        int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        partial += string(buf, n);

        size_t pos;
        while ((pos = partial.find('\n')) != string::npos) {
            string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            cout << "[RECV fd=" << clientFd << "] " << line << endl;

            istringstream iss(line);
            string cmd;
            iss >> cmd;
            string rest;
            getline(iss, rest);
            if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);


            if (cmd == "JOIN") {
                lock_guard<mutex> lk(g_mutex);
                if (self) {
                    sendLine(clientFd, "ERROR Already registered as " + self->name); continue;
                }
                if (rest.empty() || rest.find(' ') != string::npos) {
                    sendLine(clientFd, "ERROR Usage: JOIN <single_word_name>"); continue;
                }
                if (findPlayerByName(rest)) {
                    sendLine(clientFd, "ERROR Name '" + rest + "' already taken."); continue;
                }
                self = new Player(clientFd, rest);
                g_allPlayers.push_back(self);
                sendLine(clientFd, "JOINED Hello, " + rest + "! You are in the global lobby.");
                sendLine(clientFd, "INFO Next: LIST_ROOMS  or  CREATE_ROOM <id>  or  JOIN_ROOM <id>");
                cout << "[SERVER] Registered: " << rest << endl;
            }

            else if (cmd == "LIST_ROOMS") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }
                sendRoomListLocked(clientFd);
            }

            else if (cmd == "CREATE_ROOM") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }
                if (rest.empty() || rest.find(' ') != string::npos) {
                    sendLine(clientFd, "ERROR Usage: CREATE_ROOM <single_word_id>"); continue;
                }
                if (g_rooms.count(rest)) {
                    sendLine(clientFd, "ERROR Room '" + rest + "' already exists."); continue;
                }
                g_rooms[rest] = new Room(rest);
                sendLine(clientFd, "ROOM_CREATED " + rest);
                for (auto* p : g_allPlayers)
                    if (p->connected && p->room == nullptr)
                        sendRoomListLocked(p->fd);
                cout << "[SERVER] Room '" << rest << "' created by " << self->name << endl;
            }

            else if (cmd == "JOIN_ROOM") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }
                if (rest.empty()) { sendLine(clientFd, "ERROR Usage: JOIN_ROOM <id>"); continue; }
                if (self->room) {
                    sendLine(clientFd, "ERROR Already in room '" + self->room->id +
                             "'. Use LEAVE_ROOM first."); continue;
                }
                auto it = g_rooms.find(rest);
                if (it == g_rooms.end()) {
                    sendLine(clientFd, "ERROR Room '" + rest + "' not found."); continue;
                }
                Room* r = it->second;
                if (r->state == GameState::PLAYING || r->state == GameState::VOTING) {
                    sendLine(clientFd, "ERROR Room '" + rest + "' game is in progress."); continue;
                }
                self->room = r;
                r->players.push_back(self);

                sendLine(clientFd, "ROOM_JOINED " + r->id);
                r->broadcast("INFO " + self->name + " joined the room.");
                r->announcePlayerList();

                int cnt = r->connectedCount();
                sendLine(clientFd, "INFO Room [" + r->id + "] has " +
                         to_string(cnt) + " player(s). Need " +
                         to_string(MIN_PLAYERS) + " to start.");
                if (cnt >= MIN_PLAYERS)
                    r->broadcast("INFO Enough players! Game will auto-start in 3 seconds.");

                cout << "[" << r->id << "] " << self->name
                     << " joined. Players=" << cnt << endl;
            }

            else if (cmd == "LEAVE_ROOM") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }
                if (!self->room) { sendLine(clientFd, "ERROR Not in any room."); continue; }

                Room* r = self->room;
                r->players.erase(remove(r->players.begin(), r->players.end(), self),
                                 r->players.end());
                self->room = nullptr;
                self->role = "";
                self->vote = "";

                sendLine(clientFd, "ROOM_LEFT You left room '" + r->id + "'.");
                r->broadcast("INFO " + self->name + " left the room.");
                r->announcePlayerList();

                if (r->state == GameState::PLAYING && r->connectedCount() < 2) {
                    r->broadcast("INFO Too few players. Game aborted.");
                    r->state = GameState::WAITING;
                }
                cout << "[" << r->id << "] " << self->name << " left." << endl;
            }
            else if (cmd == "START") {
                Room* r = nullptr;
                {
                    lock_guard<mutex> lk(g_mutex);
                    if (!self)       { sendLine(clientFd, "ERROR JOIN first."); continue; }
                    if (!self->room) { sendLine(clientFd, "ERROR Join a room first."); continue; }
                    r = self->room;
                    if (r->state == GameState::PLAYING || r->state == GameState::VOTING) {
                        sendLine(clientFd, "ERROR Game already in progress."); continue;
                    }
                    if (r->connectedCount() < MIN_PLAYERS) {
                        sendLine(clientFd, "ERROR Need " + to_string(MIN_PLAYERS) +
                                 " players. Have " + to_string(r->connectedCount()) + "."); continue;
                    }
                }
                startRoomGame(r);
            }

            else if (cmd == "MSG") {
                lock_guard<mutex> lk(g_mutex);
                if (!self)       { sendLine(clientFd, "ERROR JOIN first."); continue; }
                if (!self->room) { sendLine(clientFd, "ERROR Join a room first."); continue; }
                Room* r = self->room;
                if (r->state != GameState::PLAYING) {
                    sendLine(clientFd, "ERROR No active game in this room."); continue;
                }
                vector<Player*> active;
                for (auto* p : r->players) if (p->connected) active.push_back(p);
                if (active.empty()) continue;

                Player* turnP = active[r->currentTurn % (int)active.size()];
                if (turnP->fd != clientFd) {
                    sendLine(clientFd, "ERROR Not your turn. Current: " + turnP->name); continue;
                }
                if (rest.empty()) { sendLine(clientFd, "ERROR Message cannot be empty."); continue; }

                r->broadcast("MSG " + self->name + ": " + rest);
                advanceTurnLocked(r);
            }

            else if (cmd == "VOTE") {
                bool  allVoted = false;
                Room* r        = nullptr;
                {
                    lock_guard<mutex> lk(g_mutex);
                    if (!self)       { sendLine(clientFd, "ERROR JOIN first."); continue; }
                    if (!self->room) { sendLine(clientFd, "ERROR Join a room first."); continue; }
                    r = self->room;
                    if (r->state != GameState::VOTING) {
                        sendLine(clientFd, "ERROR Voting is not open yet."); continue;
                    }
                    if (!self->vote.empty()) {
                        sendLine(clientFd, "ERROR Already voted for " + self->vote); continue;
                    }
                    if (rest.empty() || r->findByName(rest) == nullptr) {
                        sendLine(clientFd, "ERROR Unknown player in this room: '" + rest + "'"); continue;
                    }
                    if (rest == self->name) {
                        sendLine(clientFd, "ERROR Cannot vote for yourself."); continue;
                    }
                    self->vote = rest;
                    r->broadcast("VOTE_CAST " + self->name + " voted.");

                    int voted = 0, total = 0;
                    for (auto* p : r->players) if (p->connected) {
                        total++;
                        if (!p->vote.empty()) voted++;
                    }
                    r->broadcast("VOTE_STATUS " + to_string(voted) +
                                 "/" + to_string(total) + " votes cast.");
                    if (voted == total) allVoted = true;
                }
                if (allVoted) {
                    lock_guard<mutex> lk(g_mutex);
                    resolveVotesLocked(r);
                }
            }
            else if (cmd == "RESTART") {
                Room* r      = nullptr;
                bool doStart = false;
                {
                    lock_guard<mutex> lk(g_mutex);
                    if (!self)       { sendLine(clientFd, "ERROR JOIN first."); continue; }
                    if (!self->room) { sendLine(clientFd, "ERROR Join a room first."); continue; }
                    r = self->room;
                    if (r->state != GameState::FINISHED) {
                        sendLine(clientFd, "ERROR Game has not finished yet."); continue;
                    }
                    for (auto* p : r->players) { p->vote = ""; p->role = ""; }
                    r->state = GameState::WAITING;
                    doStart  = (r->connectedCount() >= MIN_PLAYERS);
                    if (!doStart) r->broadcast("INFO Waiting for more players...");
                }
                if (doStart) startRoomGame(r);
            }

            else if (cmd == "DM") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }

                istringstream dmiss(rest);
                string target;
                dmiss >> target;
                string dmBody;
                getline(dmiss, dmBody);
                if (!dmBody.empty() && dmBody.front() == ' ') dmBody.erase(0, 1);

                if (target.empty() || dmBody.empty()) {
                    sendLine(clientFd, "ERROR Usage: DM <player_name> <message_text>"); continue;
                }
                if (target == self->name) {
                    sendLine(clientFd, "ERROR Cannot DM yourself."); continue;
                }

                Player* dest = findPlayerByName(target);
                if (!dest) {
                    sendLine(clientFd, "ERROR Player '" + target + "' not found or offline."); continue;
                }

                sendLine(dest->fd,  "DM_FROM " + self->name + " " + dmBody);
                sendLine(clientFd,  "DM_SENT " + target + " (delivered)");

                cout << "[DM] " << self->name << " -> " << target
                     << " : " << dmBody << endl;
            }

            else if (cmd == "STATUS") {
                lock_guard<mutex> lk(g_mutex);
                if (!self) { sendLine(clientFd, "ERROR JOIN first."); continue; }

                if (!self->room) {
                    sendLine(clientFd, "STATUS In global lobby. Not in any room.");
                    sendRoomListLocked(clientFd);
                } else {
                    Room* r = self->room;
                    sendLine(clientFd, "STATUS Room=" + r->id +
                             " State=" + r->stateStr() +
                             " Players=" + to_string(r->connectedCount()));
                    r->announcePlayerList();
                }
            }

            else if (cmd == "QUIT") {
                sendLine(clientFd, "BYE Goodbye!");
                goto cleanup;
            }

            else {
                sendLine(clientFd,
                    "ERROR Unknown command '" + cmd + "'. "
                    "Valid: JOIN LIST_ROOMS CREATE_ROOM JOIN_ROOM LEAVE_ROOM "
                    "START MSG VOTE RESTART DM STATUS QUIT");
            }

        }
    } 

cleanup:
    {
        lock_guard<mutex> lk(g_mutex);
        if (self) {
            self->connected = false;
            if (self->room) {
                Room* r = self->room;
                r->players.erase(
                    remove(r->players.begin(), r->players.end(), self),
                    r->players.end());
                r->broadcast("INFO " + self->name + " disconnected.");
                r->announcePlayerList();

                if (r->state == GameState::PLAYING && r->connectedCount() < 2) {
                    r->broadcast("INFO Not enough players. Game aborted.");
                    r->state = GameState::WAITING;
                }
                self->room = nullptr;
            }
            cout << "[SERVER] '" << self->name << "' disconnected." << endl;
        }
    }
    close(clientFd);
}


int main(int argc, char* argv[])
{
    srand(static_cast<unsigned>(time(nullptr)));

    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) { perror("[SERVER] socket"); return 1; }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in saddr{};
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons(port);

    if (bind  (serverFd, (sockaddr*)&saddr, sizeof(saddr)) < 0) { perror("[SERVER] bind");   return 1; }
    if (listen(serverFd, BACKLOG)                          < 0) { perror("[SERVER] listen"); return 1; }

    cout << "Spyfall Server started on port " << port << endl;
    cout << "Min players per room: " << MIN_PLAYERS << endl;

    thread(autoStartWatcher).detach();

    while (g_serverRunning) {
        sockaddr_in caddr{};
        socklen_t   clen = sizeof(caddr);
        int         cfd  = accept(serverFd, (sockaddr*)&caddr, &clen);
        if (cfd < 0) continue;

        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &caddr.sin_addr, ipbuf, sizeof(ipbuf));
        string caddrStr = string(ipbuf) + ":" + to_string(ntohs(caddr.sin_port));

        thread(handleClient, cfd, caddrStr).detach();
    }

    close(serverFd);
    return 0;
}
