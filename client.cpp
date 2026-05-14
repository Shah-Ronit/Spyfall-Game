

#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace std;

static const int    DEFAULT_PORT = 12345;
static const char*  DEFAULT_IP   = "127.0.0.1";
static const size_t BUFFER_SIZE  = 2048;


atomic<bool> g_connected(false);
mutex        g_printMutex;
string       g_myName;
string       g_myRole;
string       g_myRoom;
atomic<bool> g_myTurn(false);
atomic<bool> g_votingOpen(false);


static void print(const string& msg)
{
    lock_guard<mutex> lk(g_printMutex);
    cout << msg << "\n";
}


static void printHelp()
{
    lock_guard<mutex> lk(g_printMutex);
    cout << "\n--- Commands ---\n"
         << "  JOIN <name>            register your player name\n"
         << "  LIST_ROOMS             see all rooms and their state\n"
         << "  CREATE_ROOM <id>       create a new room\n"
         << "  JOIN_ROOM <id>         enter a room\n"
         << "  LEAVE_ROOM             exit your current room\n"
         << "  STATUS                 show current room/game info\n"
         << "  START                  start game manually (>= 4 players)\n"
         << "  MSG <text>             speak on your turn\n"
         << "  VOTE <player>          vote during voting phase\n"
         << "  RESTART                replay after game finishes\n"
         << "  DM <player> <text>     send a private message\n"
         << "  QUIT                   disconnect\n"
         << "  help                   show this help\n"
         << "----------------\n";
}


static void handleServerMessage(const string& raw)
{
    if (raw.empty()) return;

    istringstream iss(raw);
    string cmd;
    iss >> cmd;
    string rest;
    getline(iss, rest);
    if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);

    lock_guard<mutex> lk(g_printMutex);

    if (cmd == "WELCOME") {
        cout << "[SERVER] " << rest << "\n";

    } else if (cmd == "JOINED") {
        cout << "[OK] " << rest << "\n";

    } else if (cmd == "ROOM_LIST") {
        cout << "[ROOMS] " << rest << "\n";

    } else if (cmd == "ROOM_CREATED") {
        cout << "[ROOM] Created: " << rest << "\n";

    } else if (cmd == "ROOM_JOINED") {
        g_myRoom = rest;
        cout << "[ROOM] Joined: " << rest << "\n";
        cout << "       Use MSG to speak | DM <name> to whisper | LEAVE_ROOM to exit\n";

    } else if (cmd == "ROOM_LEFT") {
        g_myRoom.clear();
        g_myTurn     = false;
        g_votingOpen = false;
        g_myRole.clear();
        cout << "[ROOM] " << rest << "\n";

    } else if (cmd == "ROOM_PLAYERS") {
        cout << "[PLAYERS in " << g_myRoom << "] " << rest << "\n";

    } else if (cmd == "INFO") {
        cout << "[INFO] " << rest << "\n";

    } else if (cmd == "ROLE") {
        istringstream rs(rest);
        string sub;
        rs >> sub;

        if (sub == "SPY") {
            g_myRole = "SPY";
            cout << "[ROLE] YOU ARE THE SPY!\n"
                 << "       Listen for clues - guess the location. Blend in.\n";
        } else if (sub == "LOCATION") {
            string loc;
            rs >> loc;
            g_myRole = "CIVILIAN";
            cout << "[ROLE] YOU ARE A CIVILIAN\n"
                 << "       Location: " << loc << "\n"
                 << "       Find the spy without revealing the location!\n";
        }

    } else if (cmd == "GAME_START") {
        cout << "\n[GAME] STARTED - " << rest << "\n"
             << "  MSG <text>    - speak on your turn\n"
             << "  VOTE <name>   - vote during voting\n"
             << "  DM <n> <msg>  - private message (any time)\n\n";

    } else if (cmd == "TURN") {
        if (rest == g_myName) {
            g_myTurn = true;
            cout << "\n[TURN] YOUR TURN - type: MSG <your message>\n";
        } else {
            g_myTurn = false;
            cout << "[TURN] " << rest << "'s turn.\n";
        }

    } else if (cmd == "MSG") {
        cout << "[MSG] " << rest << "\n";

    } else if (cmd == "VOTING_START") {
        g_votingOpen = true;
        g_myTurn     = false;
        cout << "\n[VOTE] VOTING PHASE - cast your vote with: VOTE <player_name>\n";

    } else if (cmd == "VOTE_CAST") {
        cout << "[VOTE] " << rest << "\n";

    } else if (cmd == "VOTE_STATUS") {
        cout << "[VOTE] " << rest << "\n";

    } else if (cmd == "RESULT") {
        cout << "[RESULT] " << rest << "\n";

    } else if (cmd == "REVEAL") {
        g_votingOpen = false;
        g_myTurn     = false;
        cout << "[REVEAL] " << rest << "\n";

    } else if (cmd == "DM_FROM") {
        istringstream dmiss(rest);
        string sender;
        dmiss >> sender;
        string body;
        getline(dmiss, body);
        if (!body.empty() && body.front() == ' ') body.erase(0, 1);
        cout << "[DM from " << sender << "] " << body << "\n";

    } else if (cmd == "DM_SENT") {
        cout << "[DM sent to " << rest << "]\n";

    } else if (cmd == "STATUS") {
        cout << "[STATUS] " << rest << "\n";

    } else if (cmd == "ERROR") {
        cout << "[ERROR] " << rest << "\n";

    } else if (cmd == "BYE") {
        cout << "[SERVER] " << rest << "\n";
        g_connected = false;

    } else {
        cout << "[RAW] " << raw << "\n";
    }
}


static void receiverThread(int fd)
{
    char   buf[BUFFER_SIZE];
    string partial;

    while (g_connected) {
        memset(buf, 0, sizeof(buf));
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            print("[CLIENT] Connection lost.");
            g_connected = false;
            break;
        }
        partial += string(buf, n);

        size_t pos;
        while ((pos = partial.find('\n')) != string::npos) {
            string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handleServerMessage(line);
        }
    }
}


static bool sendLine(int fd, const string& msg)
{
    string line = msg + "\n";
    int sent = ::send(fd, line.c_str(), line.size(), 0);
    if (sent <= 0) {
        print("[CLIENT] Send failed.");
        g_connected = false;
        return false;
    }
    return true;
}


static void printPrompt()
{
    lock_guard<mutex> lk(g_printMutex);
    if (g_myTurn)
        cout << "(YOUR TURN) > " << flush;
    else if (g_votingOpen)
        cout << "(VOTE) > " << flush;
    else if (!g_myRoom.empty())
        cout << "[" << g_myRoom << "] > " << flush;
    else
        cout << "(lobby) > " << flush;
}


int main(int argc, char* argv[])
{
    const char* ip   = DEFAULT_IP;
    int         port = DEFAULT_PORT;
    if (argc >= 2) ip   = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    cout << "Spyfall Client - Rooms + DM Edition\n";
    cout << "Connecting to " << ip << ":" << port << " ...\n";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[CLIENT] socket"); return 1; }

    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &saddr.sin_addr) <= 0) {
        cerr << "[CLIENT] Bad IP: " << ip << "\n"; return 1;
    }
    if (connect(fd, (sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("[CLIENT] connect"); return 1;
    }

    cout << "[CLIENT] Connected!\n";
    printHelp();

    g_connected = true;
    thread(receiverThread, fd).detach();

    string input;
    while (g_connected) {
        printPrompt();
        if (!getline(cin, input)) break;
        if (input.empty()) continue;

        if (input == "help" || input == "?") { printHelp(); continue; }

        istringstream iss(input);
        string cmd;
        iss >> cmd;
        string arg;
        getline(iss, arg);
        if (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);

        if (cmd == "JOIN" && !arg.empty())
            g_myName = arg;

        if (cmd == "LEAVE_ROOM") {
            g_myTurn     = false;
            g_votingOpen = false;
        }

        if (!sendLine(fd, input)) break;
        if (cmd == "QUIT") { g_connected = false; break; }
    }

    g_connected = false;
    shutdown(fd, SHUT_RDWR);
    close(fd);
    cout << "[CLIENT] Disconnected. Goodbye!\n";
    return 0;
}
