#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
using namespace std;

struct Client { int sock; int pipefd; pid_t pid; string name; };
struct Msg { char text[512]; };          // fixed size record, child -> parent

vector<Client> clients;
volatile sig_atomic_t quit_flag = 0;

void on_sigchld(int) {                   // reap finished handlers
    int e = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    errno = e;
}
void on_sigint(int) { quit_flag = 1; }

string stamp() {
    char b[8]; time_t t = time(nullptr);
    strftime(b, sizeof b, "%H:%M", localtime(&t));
    return b;
}
void send_to(int fd, const string &s) { send(fd, s.c_str(), s.size(), MSG_NOSIGNAL); }

void broadcast(const string &s, int except = -1) {
    for (auto &c : clients)
        if (!c.name.empty() && c.sock != except) send_to(c.sock, s);
}
string online_list() {
    string r = "Online: "; int n = 0;
    for (auto &c : clients)
        if (!c.name.empty()) { if (n++) r += ", "; r += c.name; }
    return r + " (" + to_string(n) + " users)\n";
}
Client *find_user(const string &name) {
    for (auto &c : clients) if (c.name == name) return &c;
    return nullptr;
}

// ---- runs in the forked child: socket -> pipe ----
void child_loop(int sock, int pw) {
    signal(SIGINT, SIG_IGN);
    send_to(sock, "Enter username: ");
    string buf; char tmp[512];
    for (;;) {
        ssize_t n = recv(sock, tmp, sizeof tmp, 0);
        if (n <= 0) _exit(0);            // client gone or server shut socket
        buf.append(tmp, n);
        size_t p;
        while ((p = buf.find('\n')) != string::npos) {
            string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            Msg m{}; strncpy(m.text, line.c_str(), sizeof m.text - 1);
            write(pw, &m, sizeof m);
        }
    }
}

// remove client i; announce if it had joined
void drop(size_t i) {
    string name = clients[i].name;
    close(clients[i].sock);
    close(clients[i].pipefd);
    clients.erase(clients.begin() + i);
    if (!name.empty()) broadcast("[Server] : " + name + " has left the chat\n");
}

// returns true if client i was removed
bool handle(size_t i) {
    Msg m;
    if (read(clients[i].pipefd, &m, sizeof m) != (ssize_t)sizeof m) { drop(i); return true; }
    m.text[sizeof m.text - 1] = 0;
    string line = m.text;
    Client &c = clients[i];

    if (c.name.empty()) {                // still choosing username
        if (line.find(' ') != string::npos || line.size() > 20) {
            send_to(c.sock, "Invalid name (no spaces, max 20 chars). Enter username: ");
        } else if (find_user(line)) {
            send_to(c.sock, "Username already taken. Enter username: ");
        } else {
            c.name = line;
            broadcast("[Server] : " + c.name + " has joined the chat\n", c.sock);
            send_to(c.sock, "Welcome! Commands: /list  /msg <user> <text>  /quit\n" + online_list());
        }
    } else if (line == "/list") {
        send_to(c.sock, online_list());
    } else if (line == "/quit") {
        shutdown(c.sock, SHUT_RDWR);     // child sees EOF, exits, pipe EOF -> drop()
    } else if (line.rfind("/msg ", 0) == 0) {
        size_t sp = line.find(' ', 5);
        if (sp == string::npos) { send_to(c.sock, "Usage: /msg <user> <text>\n"); }
        else {
            Client *t = find_user(line.substr(5, sp - 5));
            if (!t) send_to(c.sock, "[Server] : no such user\n");
            else {
                string body = line.substr(sp + 1);
                send_to(t->sock, "[" + stamp() + "] [PM from " + c.name + "]: " + body + "\n");
                send_to(c.sock, "[" + stamp() + "] [PM to " + t->name + "]: " + body + "\n");
            }
        }
    } else {
        broadcast("[" + stamp() + "] [" + c.name + "]: " + line + "\n");
    }
    return false;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 5000;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(lfd, (sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 10) < 0) { perror("listen"); return 1; }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{}; sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; sigaction(SIGCHLD, &sa, nullptr);
    struct sigaction si{}; si.sa_handler = on_sigint; sigaction(SIGINT, &si, nullptr);

    printf("SocketChat server listening on port %d\n", port);

    while (!quit_flag) {
        fd_set rf; FD_ZERO(&rf); FD_SET(lfd, &rf); int maxfd = lfd;
        for (auto &c : clients) { FD_SET(c.pipefd, &rf); if (c.pipefd > maxfd) maxfd = c.pipefd; }
        if (select(maxfd + 1, &rf, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (FD_ISSET(lfd, &rf)) {
            sockaddr_in ca; socklen_t cl = sizeof ca;
            int cs = accept(lfd, (sockaddr *)&ca, &cl);
            if (cs >= 0) {
                int p[2];
                if (pipe(p) < 0) { close(cs); }
                else {
                    pid_t pid = fork();
                    if (pid == 0) {                       // child
                        close(lfd); close(p[0]);
                        for (auto &c : clients) { close(c.sock); close(c.pipefd); }
                        child_loop(cs, p[1]);
                    }
                    close(p[1]);                          // parent
                    clients.push_back({cs, p[0], pid, ""});
                    printf("Connection from %s\n", inet_ntoa(ca.sin_addr));
                }
            }
        }
        for (size_t i = 0; i < clients.size();)
            if (FD_ISSET(clients[i].pipefd, &rf) && handle(i)) continue; else i++;
    }

    printf("\nShutting down...\n");
    broadcast("[Server] : server is shutting down\n");
    for (auto &c : clients) { kill(c.pid, SIGTERM); close(c.sock); close(c.pipefd); }
    close(lfd);
    while (waitpid(-1, nullptr, 0) > 0) {}
    return 0;
}