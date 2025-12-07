#include<iostream>
#include<thread>
#include<string>
#include<cstring>
#include<ctime>
#include<mutex>
#include<unistd.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<fstream>
using namespace std;
const int TCP_port = 5000;
const int UDP_port = 6000;
const int MAX_CLIENTS = 10;
const int BUF = 8192;               // buffer size for reads
const int MAX_FILES = 200;          // max number of received files the server will index

// Hard-coded credentials (campus -> pass).
struct Cred { const char* campus; const char* pass; };
Cred creds[] = { {"Lahore","NU-LHR-123"}, {"Karachi","NU-KHI-123"}, {"Multan","NU-MULT-123"}, {"Peshawar","NU-PSH-123"}, {"CFD","NU-CFD-123"} };
int CRED_COUNT = sizeof(creds)/sizeof(creds[0]);

// Simple client slot (fixed array; no STL containers)
struct client_slot {
    bool used;                 // slot in use?
    char name[64];             // campus name
    int tcpSock;               // TCP socket fd (-1 if none)
    sockaddr_in udpAddr;       // last UDP heartbeat sender address
    time_t lastHB;             // last heartbeat time (0 if none)
    client_slot() { used=false; tcpSock=-1; lastHB=0; memset(name,0,sizeof(name)); memset(&udpAddr,0,sizeof(udpAddr)); }
} clients[MAX_CLIENTS];

mutex mtx; // protects clients[] and files list and other shared state

// Maintain a simple index of received files (so admin can list & open them)
struct rcvd_file{
    bool used;
    char storedName[256];    // saved filename on server (received_from_<campus>_<filename>)
    char originalName[128];  // original filename sent by client
    char sender[64];         // sender campus
    time_t receivedAt;
    rcvd_file() { used=false; memset(storedName,0,sizeof(storedName)); memset(originalName,0,sizeof(originalName)); memset(sender,0,sizeof(sender)); receivedAt=0; }
} receivedFiles[MAX_FILES];

// Simple log with timestamp
void login(const string &s) {
    time_t t = time(NULL);
    char tb[26]; ctime_r(&t, tb); tb[strlen(tb)-1] = 0; // remove newline
    cout << "[" << tb << "] " << s << endl;
}

// Validate auth string of form "Campus:Name;Pass:Pwd"
// Returns true + sets campusOut if valid. Also rejects "Islamabad".
bool validateAuth(const string &msg, string &campusOut) {
    size_t p1 = msg.find("Campus:");
    size_t p2 = msg.find(";Pass:");
    if (p1 == string::npos || p2 == string::npos) return false;
    string camp = msg.substr(p1+7, p2-(p1+7));
    string pass = msg.substr(p2+6);
    // trim spaces
    while(!camp.empty() && isspace(camp.back())) camp.pop_back();
    while(!camp.empty() && isspace(camp.front())) camp.erase(0,1);

    if (camp == "Islamabad") return false; // server itself; cannot be client

    for (int i=0;i<CRED_COUNT;i++) {
        if (camp == creds[i].campus && pass == creds[i].pass) { campusOut = camp; return true; }
    }
    return false;
}

// Find index by campus name, or -1 if not present
int findClientByName(const string &name) {
    for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].used && name == string(clients[i].name)) return i;
    return -1;
}

// Find an empty client slot index or -1
int findEmptySlot() {
    for (int i=0;i<MAX_CLIENTS;i++) if (!clients[i].used) return i;
    return -1;
}

// Add a received file to index; returns true if added
bool indexReceivedFile(const string &storedName, const string &origName, const string &sender) {
    for (int i=0;i<MAX_FILES;i++) {
        if (!receivedFiles[i].used) {
            receivedFiles[i].used = true;
            strncpy(receivedFiles[i].storedName, storedName.c_str(), sizeof(receivedFiles[i].storedName)-1);
            strncpy(receivedFiles[i].originalName, origName.c_str(), sizeof(receivedFiles[i].originalName)-1);
            strncpy(receivedFiles[i].sender, sender.c_str(), sizeof(receivedFiles[i].sender)-1);
            receivedFiles[i].receivedAt = time(NULL);
            return true;
        }
    }
    return false;
}

// Handle one TCP client connection: authenticate, then receive SEND and FILE commands
void handleClient(int clientSock) {
    char buf[BUF];
    memset(buf,0,sizeof(buf));
    ssize_t r = read(clientSock, buf, sizeof(buf)-1);
    if (r <= 0) { close(clientSock); return; }
    string auth(buf);
    string campus;
    if (!validateAuth(auth, campus)) {
        // Before refusing, check if it was "Islamabad" attempt or bad creds
        // Also check duplicate login
        mtx.lock();
        bool duplicate = (findClientByName(campus) != -1);
        mtx.unlock();
        if (duplicate) {
            string resp = "AUTH_FAIL_DUPLICATE";
            write(clientSock, resp.c_str(), resp.size());
            login("Rejected duplicate login attempt for " + campus);
        } else {
            string resp = "AUTH_FAIL";
            write(clientSock, resp.c_str(), resp.size());
            login("Rejected authentication (bad creds or Islamabad attempt).");
        }
        close(clientSock);
        return;
    }

    // Prevent duplicate login even if creds were correct: check again under lock
    mtx.lock();
    if (findClientByName(campus) != -1) {
        mtx.unlock();
        string resp = "AUTH_FAIL_DUPLICATE";
        write(clientSock, resp.c_str(), resp.size());
        login("Rejected duplicate login attempt for " + campus);
        close(clientSock);
        return;
    }
    // register in clients[]
    int idx = findEmptySlot();
    if (idx == -1) {
        mtx.unlock();
        string resp = "SERVER_FULL";
        write(clientSock, resp.c_str(), resp.size());
        login("Rejected auth: server full for " + campus);
        close(clientSock);
        return;
    }
    clients[idx].used = true;
    strncpy(clients[idx].name, campus.c_str(), sizeof(clients[idx].name)-1);
    clients[idx].tcpSock = clientSock;
    clients[idx].lastHB = 0; // will be updated when UDP heartbeat arrives
    memset(&clients[idx].udpAddr, 0, sizeof(clients[idx].udpAddr));
    mtx.unlock();

    login("Authenticated and connected TCP: " + campus);
    write(clientSock, string("AUTH_OK").c_str(), 7);

    // Main loop: read commands from client
    while (true) {
        memset(buf,0,sizeof(buf));
        ssize_t n = read(clientSock, buf, sizeof(buf)-1);
        if (n <= 0) break; // disconnected

        string inc(buf);
        // Two supported patterns:
        // 1) SEND|Target|Message
        // 2) FILE|Target|Filename|<content>
        if (inc.rfind("SEND|",0) == 0) {
            size_t p1 = inc.find("|",5);
            if (p1 == string::npos) { write(clientSock, "BAD_FORMAT",9); continue; }
            string target = inc.substr(5, p1-5);
            string text = inc.substr(p1+1);

            if (target == "Islamabad") {
                // Message intended to server => show it on server console explicitly
                login("MESSAGE TO SERVER from " + campus + ": " + text);
                write(clientSock, "DELIVERED_TO_SERVER",18);
            } else {
                mtx.lock();
                int tid = findClientByName(target);
                if (tid != -1 && clients[tid].used && clients[tid].tcpSock != -1) {
                    string fwd = "From " + campus + ": " + text;
                    write(clients[tid].tcpSock, fwd.c_str(), fwd.size());
                    write(clientSock, "DELIVERED",9);
                    login("Routed message from " + campus + " to " + target);
                } else {
                    write(clientSock, "TARGET_OFFLINE",14);
                    login("Failed to route message from " + campus + " to " + target + " (offline).");
                }
                mtx.unlock();
            }
        }
        else if (inc.rfind("FILE|",0) == 0) {
            // parse: FILE|Target|Filename|<content>
            size_t p1 = inc.find("|",5);
            size_t p2 = string::npos;
            if (p1 != string::npos) p2 = inc.find("|", p1+1);
            if (p1==string::npos || p2==string::npos) {
                write(clientSock, "INVALID_FILE_FORMAT",19);
                continue;
            }
            string target = inc.substr(5, p1-5);
            string fname = inc.substr(p1+1, p2-(p1+1));
            string content = inc.substr(p2+1);

            if (target == "Islamabad") {
                // Save file on server disk
                string stored = "received_from_" + campus + "_" + fname;
                ofstream ofs(stored.c_str(), ios::out | ios::binary);
                if (!ofs) {
                    write(clientSock, "SERVER_SAVE_ERR",16);
                    login("Error saving file from " + campus + ": " + fname);
                } else {
                    ofs << content;
                    ofs.close();
                    mtx.lock();
                    indexReceivedFile(stored, fname, campus);
                    mtx.unlock();
                    write(clientSock, "FILE_SAVED_ON_SERVER",20);
                    login("Saved file from " + campus + " as " + stored);
                }
            } else {
                // Forward file to target client if connected
                mtx.lock();
                int tid = findClientByName(target);
                if (tid != -1 && clients[tid].used && clients[tid].tcpSock != -1) {
                    // forward raw packet exactly as received
                    write(clients[tid].tcpSock, inc.c_str(), inc.size());
                    write(clientSock, "FILE_FORWARDED",14);
                    login("Forwarded file '" + fname + "' from " + campus + " to " + target);
                } else {
                    write(clientSock, "TARGET_OFFLINE",14);
                    login("File forward failed from " + campus + " to " + target + " (offline).");
                }
                mtx.unlock();
            }
        }
        else {
            write(clientSock, "UNKNOWN_CMD",11);
        }
    }

    // cleanup on disconnect
    mtx.lock();
    int idxNow = findClientByName(campus);
    if (idxNow != -1) {
        clients[idxNow].used = false;
        clients[idxNow].tcpSock = -1;
        clients[idxNow].lastHB = 0;
        memset(&clients[idxNow].udpAddr, 0, sizeof(clients[idxNow].udpAddr));
        memset(clients[idxNow].name, 0, sizeof(clients[idxNow].name));
    }
    mtx.unlock();
    login("Client disconnected: " + campus);
    close(clientSock);
}

// UDP listener: receives heartbeats like "Campus:Name;HB:online",
 // stores sender address and updates lastHB. Will register UDP-only clients if needed.
void udpListener() {
    int usock = socket(AF_INET, SOCK_DGRAM, 0);
    if (usock < 0) { login("UDP socket create failed"); return; }
    sockaddr_in addr; addr.sin_family = AF_INET; addr.sin_port = htons(UDP_port); addr.sin_addr.s_addr = INADDR_ANY;
    bind(usock, (sockaddr*)&addr, sizeof(addr));
    login("UDP listening on port " + to_string(UDP_port));

    char buf[BUF];
    while (true) {
        memset(buf,0,sizeof(buf));
        sockaddr_in sender; socklen_t sl = sizeof(sender);
        ssize_t r = recvfrom(usock, buf, sizeof(buf)-1, 0, (sockaddr*)&sender, &sl);
        if (r <= 0) continue;
        string msg(buf);
        size_t p = msg.find("Campus:");
        if (p == string::npos) continue;
        size_t semi = msg.find(";", p);
        string name = (semi==string::npos) ? msg.substr(p+7) : msg.substr(p+7, semi-(p+7));

        // update clients[] info or register as UDP-only
        mtx.lock();
        int idx = findClientByName(name);
        if (idx != -1) {
            clients[idx].lastHB = time(NULL);
            clients[idx].udpAddr = sender;
        } else {
            int e = findEmptySlot();
            if (e != -1) {
                clients[e].used = true;
                strncpy(clients[e].name, name.c_str(), sizeof(clients[e].name)-1);
                clients[e].tcpSock = -1; // UDP-only for now
                clients[e].lastHB = time(NULL);
                clients[e].udpAddr = sender;
                login("Registered UDP-only campus: " + name);
            } else {
                login("No slot free to register heartbeat from " + name);
            }
        }
        mtx.unlock();
    }
    close(usock);
}

// Heartbeat summary printer: prints summary only when >=1 client is registered.
// Stops printing once all disconnect (i.e., only prints if at least one clients[].used == true).
void hbPrinter() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(10)); // print every 10 seconds
        mtx.lock();
        bool any = false;
        for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].used) { any = true; break; }
        if (!any) { mtx.unlock(); continue; } // skip printing if no clients
        cout << "\n===== HEARTBEAT SUMMARY =====\n";
        time_t now = time(NULL);
        for (int i=0;i<MAX_CLIENTS;i++) {
            if (clients[i].used) {
                cout << "["<<i<<"] " << clients[i].name;
                if (clients[i].tcpSock != -1) cout << " (TCP)";
                else cout << " (UDP-only)";
                if (clients[i].lastHB == 0) cout << " | lastHB: never\n";
                else cout << " | lastHB: " << (now - clients[i].lastHB) << "s ago\n";
            }
        }
        cout << "=============================\n";
        mtx.unlock();
    }
}

// Admin console (in server terminal) with options:
// 1. View clients
// 2. Broadcast announcement (UDP) to all known clients that sent heartbeat
// 3. List & open received files (show content in console)
// 4. Exit server
void adminConsole() {
    while (true) {
        cout << "\n--- ADMIN MENU ---\n1) View clients\n2) Broadcast announcement\n3) List received files\n4) Open a received file\n5) Exit\nChoice: ";
        int ch;
        if (!(cin >> ch)) { cin.clear(); string dum; getline(cin,dum); continue; }
        cin.ignore(); // remove newline
        if (ch == 1) {
            mtx.lock();
            cout << "\nIndex | Campus       | TCP? | LastHB(sec ago)\n----------------------------------------------\n";
            time_t now = time(NULL);
            for (int i=0;i<MAX_CLIENTS;i++) {
                if (clients[i].used) {
                    cout << i << "     | " << clients[i].name;
                    if (clients[i].tcpSock != -1) cout << " | Y ";
                    else cout << " | N ";
                    if (clients[i].lastHB == 0) cout << " | never\n";
                    else cout << " | " << (now - clients[i].lastHB) << "s\n";
                }
            }
            mtx.unlock();
        }
        else if (ch == 2) {
            cout << "Enter announcement text: ";
            string ann;
            getline(cin, ann);
            // Send UDP to all clients that have lastHB != 0 (we have sender address)
            int usock = socket(AF_INET, SOCK_DGRAM, 0);
            if (usock < 0) { cout << "UDP socket error\n"; continue; }
            mtx.lock();
            int sentCount = 0;
            for (int i=0;i<MAX_CLIENTS;i++) {
                if (clients[i].used && clients[i].lastHB != 0) {
                    sendto(usock, ann.c_str(), ann.size(), 0, (sockaddr*)&clients[i].udpAddr, sizeof(clients[i].udpAddr));
                    sentCount++;
                }
            }
            mtx.unlock();
            close(usock);
            login("Admin broadcast sent to " + to_string(sentCount) + " clients.");
        }
        else if (ch == 3) {
            mtx.lock();
            cout << "\n---- Received Files Index ----\n";
            for (int i=0;i<MAX_FILES;i++) {
                if (receivedFiles[i].used) {
                    time_t t = receivedFiles[i].receivedAt;
                    char tb[26]; ctime_r(&t,tb); tb[strlen(tb)-1]=0;
                    cout << i << ") " << receivedFiles[i].storedName << " (from " << receivedFiles[i].sender << ") at " << tb << "\n";
                }
            }
            mtx.unlock();
        }
        else if (ch == 4) {
            cout << "Enter index of received file to open (see list): ";
            int idx; if (!(cin >> idx)) { cin.clear(); string d; getline(cin,d); cout<<"Invalid index\n"; continue; }
            cin.ignore();
            mtx.lock();
            if (idx < 0 || idx >= MAX_FILES || !receivedFiles[idx].used) {
                mtx.unlock();
                cout << "Invalid file index\n";
                continue;
            }
            string path = receivedFiles[idx].storedName;
            mtx.unlock();
            // open and print content
            ifstream ifs(path.c_str(), ios::in | ios::binary);
            if (!ifs) { cout << "Failed to open file: " << path << "\n"; continue; }
            cout << "\n----- Content of " << path << " -----\n";
            string line;
            while (getline(ifs, line)) cout << line << "\n";
            cout << "----- End of file -----\n";
            ifs.close();
        }
        else if (ch == 5) {
            login("Admin requested exit. Shutting down.");
            exit(0);
        }
        else {
            cout << "Invalid choice\n";
        }
    }
}
int main() {
    // Start UDP listener thread (heartbeat)
    thread udpThread(udpListener);
    udpThread.detach();

    // Start heartbeat summary printer
    thread hbThread(hbPrinter);
    hbThread.detach();

    // Start admin console
    thread adminThread(adminConsole);
    adminThread.detach();

    // Start TCP listener
    int ss = socket(AF_INET, SOCK_STREAM, 0);
    if (ss < 0) { cerr << "TCP socket create failed\n"; return 1; }
    sockaddr_in saddr; saddr.sin_family = AF_INET; saddr.sin_port = htons(TCP_port); saddr.sin_addr.s_addr = INADDR_ANY;
    int opt = 1; setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(ss, (sockaddr*)&saddr, sizeof(saddr)) < 0) { cerr << "Bind failed\n"; return 1; }
    if (listen(ss, 5) < 0) { cerr << "Listen failed\n"; return 1; }
    login("TCP listening on port " + to_string(TCP_port));

    // Accept loop
    while (true) {
        sockaddr_in clientAddr; socklen_t cl = sizeof(clientAddr);
        int cs = accept(ss, (sockaddr*)&clientAddr, &cl);
        if (cs < 0) { login("Accept failed"); continue; }
        // Spawn a handler thread
        thread t(handleClient, cs);
        t.detach();
    }
    close(ss);
    return 0;
}

