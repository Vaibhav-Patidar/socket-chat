#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace std;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &a.sin_addr) != 1) { fprintf(stderr, "Bad IP\n"); return 1; }
    if (connect(s, (sockaddr *)&a, sizeof a) < 0) { perror("connect"); return 1; }

    char buf[1024];
    for (;;) {
        fd_set rf; FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf); FD_SET(s, &rf);
        if (select(s + 1, &rf, nullptr, nullptr, nullptr) < 0) { perror("select"); break; }

        if (FD_ISSET(s, &rf)) {
            ssize_t n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) { printf("\nDisconnected from server.\n"); break; }
            fwrite(buf, 1, n, stdout); fflush(stdout);
        }
        if (FD_ISSET(STDIN_FILENO, &rf)) {
            if (!fgets(buf, sizeof buf, stdin)) break;       // Ctrl-D
            send(s, buf, strlen(buf), MSG_NOSIGNAL);
            if (strncmp(buf, "/quit", 5) == 0) { /* server closes us */ }
        }
    }
    close(s);
    return 0;
}