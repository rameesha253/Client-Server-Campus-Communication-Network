#include<iostream>
#include<thread>
#include<string>
#include<cstring>
#include<mutex>
#include<fstream>
#include<unistd.h>
#include<arpa/inet.h>
#include <netinet/in.h>
using namespace std;
const int TCP_port = 5000;
const int UDP_port = 6000;
const int BUF = 8192;

// We will store up to 100 received files
struct RecFile {
    bool used;
    char storedName[256];   // name on disk
} recFiles[100];

mutex fileMtx; // protect recFiles[]

string CAMPUS; // current campus name after login

// Save a received file to disk and index it
void saveReceivedFile(const string &sender, const string &filename, const string &content) {
    // Save as: received_<sender>_<filename>
    string stored = "received_" + sender + "_" + filename;

    ofstream ofs(stored.c_str(), ios::out | ios::binary);
    if (!ofs) {
        cout << "Error saving received file.\n";
        return;
    }
    ofs << content;
    ofs.close();

    fileMtx.lock();
    for (int i=0;i<100;i++) {
        if (!recFiles[i].used) {
            recFiles[i].used = true;
            strncpy(recFiles[i].storedName, stored.c_str(), sizeof(recFiles[i].storedName)-1);
            break;
        }
    }
    fileMtx.unlock();

    cout << "File saved as: " << stored << endl;
}

// TCP listener: receives forwarded messages or files
void tcpListener(int sock) {
    char buf[BUF];

    while (true) {
        memset(buf,0,sizeof(buf));
        ssize_t r = read(sock, buf, sizeof(buf)-1);
        if (r <= 0) {
            cout << "\nDisconnected from server.\n";
            exit(0);
        }

        string inc(buf);

        // FILE packet: FILE|Source|Filename|<content>
        if (inc.rfind("FILE|",0)==0) {
            size_t p1 = inc.find("|",5);
            size_t p2 = (p1==string::npos? string::npos : inc.find("|", p1+1));
            if (p1==string::npos || p2==string::npos) continue;

            string sender = inc.substr(5, p1-5);
            string fname = inc.substr(p1+1, p2-(p1+1));
            string content = inc.substr(p2+1);

            cout << "\n--- File received from " << sender << ": " << fname << " ---\n";
            saveReceivedFile(sender, fname, content);
            cout << "--- End File ---\n";
        }
        else {
            // Normal text message
            cout << "\n" << inc << endl;
        }
    }
}

// Send heartbeat via UDP every 5 sec
void hbThreadFunc(int udpSock, sockaddr_in serv) {
    while (true) {
        string hb = "Campus:" + CAMPUS + ";HB:online";
        sendto(udpSock, hb.c_str(), hb.size(), 0, (sockaddr*)&serv, sizeof(serv));
        sleep(5);
    }
}

// UDP listener for admin announcements
void udpListener(int udpSock) {
    char buf[BUF];
    while (true) {
        memset(buf,0,sizeof(buf));
        sockaddr_in sender; socklen_t sl = sizeof(sender);
        ssize_t r = recvfrom(udpSock, buf, sizeof(buf)-1, 0, (sockaddr*)&sender, &sl);
        if (r>0) {
            cout << "\n[ADMIN BROADCAST]: " << buf << endl;
        }
    }
}

// Create new text file (simple)
bool createNewFile(string &outFilename) {
    cout << "Enter new filename (e.g. myfile.txt): ";
    getline(cin, outFilename);

    ofstream ofs(outFilename.c_str());
    if (!ofs) {
        cout << "Error creating file.\n";
        return false;
    }

    cout << "Enter text content (end with a line containing only '#'):\n";
    string line;
    while (true) {
        getline(cin,line);
        if (line=="#") break;
        ofs << line << "\n";
    }
    ofs.close();
    return true;
}

// Load existing file fully
bool loadFileContent(const string &fname, string &content) {
    ifstream ifs(fname.c_str());
    if (!ifs) return false;

    string line;
    content="";
    while (getline(ifs,line)) {
        content += line + "\n";
    }
    ifs.close();
    return true;
}

// Main client
int main() {
    memset(recFiles,0,sizeof(recFiles));

    // Get campus name
    cout << "Enter Campus Name: ";
    getline(cin, CAMPUS);

    if (CAMPUS=="Islamabad") {
        cout << "Error: Server campus cannot connect as client.\n";
        return 0;
    }

    // Get password
    cout << "Enter Password: ";
    string pass;
    getline(cin, pass);

    // TCP connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock<0) { cout << "TCP socket error.\n"; return 0; }

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(TCP_port);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(sock, (sockaddr*)&server, sizeof(server))<0) {
        cout << "Connect error.\n"; return 0;
    }

    // AUTH packet
    string auth = "Campus:" + CAMPUS + ";Pass:" + pass;
    write(sock, auth.c_str(), auth.size());

    char buf[BUF];
    memset(buf,0,sizeof(buf));
    ssize_t r = read(sock, buf, sizeof(buf)-1);
    if (r <= 0) {
        cout << "Auth response read error.\n"; return 0;
    }

    string resp(buf);
    if (resp=="AUTH_FAIL") {
        cout << "Authentication failed.\n";
        return 0;
    }
    if (resp=="AUTH_FAIL_DUPLICATE") {
        cout << "Campus already connected from another client.\n";
        return 0;
    }
    if (resp=="SERVER_FULL") {
        cout << "Server full.\n";
        return 0;
    }
    if (resp!="AUTH_OK") {
        cout << "Unexpected server response.\n";
        return 0;
    }

    cout << "\nAuthenticated successfully.\n";

    // Start TCP listener
    thread t1(tcpListener, sock);
    t1.detach();

    // Prepare UDP for heartbeat and broadcasts
    int udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock < 0) { cout<<"UDP socket error\n"; return 0;}

    sockaddr_in servHB;
    servHB.sin_family = AF_INET;
    servHB.sin_port = htons(UDP_port);
    inet_pton(AF_INET, "127.0.0.1", &servHB.sin_addr);

    // start heartbeat thread
    thread t2(hbThreadFunc, udpSock, servHB);
    t2.detach();

    // start udp listener
    thread t3(udpListener, udpSock);
    t3.detach();

    // Main menu
    while (true) {
        cout << "\n--- CLIENT MENU ("<<CAMPUS<<") ---\n";
        cout << "1) Send message to campus\n";
        cout << "2) Send text file to campus\n";
        cout << "3) Open a received file\n";
        cout << "4) Exit\n";
        cout << "Choice: ";

        string choice;
        getline(cin, choice);

        if (choice=="1") {
            cout << "Enter target campus: ";
            string target; getline(cin,target);

            cout << "Enter message text: ";
            string msg; getline(cin,msg);

            string packet = "SEND|" + target + "|" + msg;
            write(sock, packet.c_str(), packet.size());
        }
        else if (choice=="2") {
            cout << "Send file to which campus? ";
            string target; getline(cin,target);

            cout << "Choose:\n1) Use existing file\n2) Create new file\nSelection: ";
            string sel; getline(cin,sel);

            string actualFile;
            string content;

            if (sel=="1") {
                cout << "Enter existing filename: ";
                getline(cin, actualFile);

                if (!loadFileContent(actualFile, content)) {
                    cout << "File does not exist or cannot be read.\n";
                    continue;    // **IMPORTANT** return to menu cleanly
                }
            }
            else if (sel=="2") {
                if (!createNewFile(actualFile)) {
                    cout << "Error creating file.\n";
                    continue;
                }
                if (!loadFileContent(actualFile, content)) {
                    cout << "Error reloading created file.\n";
                    continue;
                }
            }
            else {
                cout << "Invalid option.\n";
                continue;
            }

            // Construct file packet
            string packet = "FILE|" + target + "|" + actualFile + "|" + content;
            write(sock, packet.c_str(), packet.size());

            cout << "File sent.\n";

            // Safely return to menu
            cin.clear();
        }
        else if (choice=="3") {
            // List received files
            fileMtx.lock();
            cout << "\n--- Received Files ---\n";
            for (int i=0;i<100;i++) {
                if (recFiles[i].used) {
                    cout << i << ") " << recFiles[i].storedName << endl;
                }
            }
            fileMtx.unlock();

            cout << "Enter index to open: ";
            string idxs; getline(cin, idxs);
            int idx = atoi(idxs.c_str());

            fileMtx.lock();
            if (idx<0 || idx>=100 || !recFiles[idx].used) {
                fileMtx.unlock();
                cout << "Invalid index.\n";
                continue;
            }
            string filepath = recFiles[idx].storedName;
            fileMtx.unlock();

            ifstream ifs(filepath.c_str());
            if (!ifs) {
                cout << "Error opening file.\n";
                continue;
            }

            cout << "\n----- File Content ("<<filepath<<") -----\n";
            string line;
            while (getline(ifs,line)) {
                cout << line << endl;
            }
            cout << "----- End -----\n";
            ifs.close();
        }
        else if (choice=="4") {
            cout << "Exiting client.\n";
            exit(0);
        }
        else {
            cout << "Invalid choice.\n";
        }
    }

    return 0;
}

