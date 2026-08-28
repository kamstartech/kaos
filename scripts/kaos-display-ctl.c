#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CTL_SOCKET_PATH "/dev/socket/kaos_display_ctl.sock"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <show|hide|quit>\n", argv[0]);
        return 1;
    }

    uint8_t cmd;
    if (strcmp(argv[1], "show") == 0) cmd = 0x01;
    else if (strcmp(argv[1], "hide") == 0) cmd = 0x02;
    else if (strcmp(argv[1], "quit") == 0) cmd = 0xFF;
    else { printf("Unknown: %s\n", argv[1]); return 1; }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (sendto(fd, &cmd, 1, 0, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("sendto");
        close(fd);
        return 1;
    }
    printf("Sent %s (0x%02x)\n", argv[1], cmd);
    close(fd);
    return 0;
}
