#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* kaos: Unified Kaos Bridge Client */

#define BRIDGE_PORT 30000
#define BRIDGE_ADDR "127.0.0.1"

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server;
    char buffer[4096];
    char command[4096] = {0};

    if (argc == 1) {
        snprintf(command, sizeof(command), "runcon u:r:su:s0 /system/bin/kaos-starter start\n");
    } else {
        strcat(command, "runcon u:r:su:s0 ");
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-c") == 0) continue;
            strcat(command, argv[i]);
            strcat(command, " ");
        }
        strcat(command, "\n");
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }

    server.sin_addr.s_addr = inet_addr(BRIDGE_ADDR);
    server.sin_family = AF_INET;
    server.sin_port = htons(BRIDGE_PORT);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        fprintf(stderr, "[kaos] Error: Root Bridge not running on port %d\n", BRIDGE_PORT);
        return 1;
    }

    if (send(sock, command, strlen(command), 0) < 0) {
        perror("send");
        return 1;
    }

    int n;
    while ((n = recv(sock, buffer, sizeof(buffer)-1, 0)) > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    }

    close(sock);
    return 0;
}
