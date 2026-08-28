#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* kaos-touch-input: Virtual Touchscreen Daemon
 *
 * Creates a uinput virtual touchscreen (MT type B + INPUT_PROP_DIRECT) and
 * injects multitouch events received from PhoshDisplayActivity via Unix socket.
 * Phoc picks up this device through its libinput backend, producing native
 * wl_touch events — no mouse cursor, true touch behavior.
 *
 * Packet format: int32_t[4] = {action, pointer_id, x, y}
 *   action 0 = DOWN   — finger touched
 *   action 1 = UP     — finger lifted
 *   action 2 = MOVE   — finger moved
 *
 * Requires: WLR_BACKENDS=headless,libinput, seatd, udevd
 * Compile:  gcc -o kaos-touch-input kaos-input.c -O2
 */

#define SOCKET_PATH "/dev/socket/kaos_input.sock"
#define MAX_SLOTS   10
#define DISPLAY_W   1080
#define DISPLAY_H   2340

static int uinput_fd = -1;
/* Maps Android pointer IDs → MT slots */
static int slot_owner[MAX_SLOTS];

static void emit(uint16_t type, uint16_t code, int32_t val) {
    struct input_event ev = {0};
    ev.type = type;
    ev.code = code;
    ev.value = val;
    write(uinput_fd, &ev, sizeof(ev));
}

static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "kaos-touch");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x5678;
    setup.id.version = 1;

    struct uinput_abs_setup abs_x    = { .code = ABS_X,              .absinfo = { .maximum = DISPLAY_W } };
    struct uinput_abs_setup abs_y    = { .code = ABS_Y,              .absinfo = { .maximum = DISPLAY_H } };
    struct uinput_abs_setup mt_slot  = { .code = ABS_MT_SLOT,        .absinfo = { .maximum = MAX_SLOTS - 1 } };
    struct uinput_abs_setup mt_id    = { .code = ABS_MT_TRACKING_ID, .absinfo = { .maximum = 65535 } };
    struct uinput_abs_setup mt_x     = { .code = ABS_MT_POSITION_X,  .absinfo = { .maximum = DISPLAY_W } };
    struct uinput_abs_setup mt_y     = { .code = ABS_MT_POSITION_Y,  .absinfo = { .maximum = DISPLAY_H } };

    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);
    ioctl(fd, UI_ABS_SETUP, &mt_slot);
    ioctl(fd, UI_ABS_SETUP, &mt_id);
    ioctl(fd, UI_ABS_SETUP, &mt_x);
    ioctl(fd, UI_ABS_SETUP, &mt_y);

    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); close(fd); return -1; }
    return fd;
}

static int find_slot(int ptr_id) {
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slot_owner[i] == ptr_id) return i;
    return -1;
}

static int alloc_slot(int ptr_id) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slot_owner[i] == -1) {
            slot_owner[i] = ptr_id;
            return i;
        }
    }
    return -1;
}

static int active_touches(void) {
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slot_owner[i] != -1) n++;
    return n;
}

int main(void) {
    for (int i = 0; i < MAX_SLOTS; i++) slot_owner[i] = -1;

    uinput_fd = setup_uinput();
    if (uinput_fd < 0) return 1;
    fprintf(stderr, "kaos-touch-input: uinput touchscreen created\n");

    unlink(SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    chmod(SOCKET_PATH, 0666);
    listen(server_fd, 1);
    fprintf(stderr, "kaos-touch-input: listening on %s\n", SOCKET_PATH);

    int32_t pkt[4];
    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        fprintf(stderr, "kaos-touch-input: client connected\n");

        ssize_t n;
        while ((n = read(client, pkt, sizeof(pkt))) == sizeof(pkt)) {
            int32_t action = pkt[0], ptr_id = pkt[1], x = pkt[2], y = pkt[3];
            int slot;

            switch (action) {
            case 0: /* DOWN */
                slot = alloc_slot(ptr_id);
                if (slot < 0) break;
                emit(EV_ABS, ABS_MT_SLOT, slot);
                emit(EV_ABS, ABS_MT_TRACKING_ID, ptr_id);
                emit(EV_ABS, ABS_MT_POSITION_X, x);
                emit(EV_ABS, ABS_MT_POSITION_Y, y);
                if (active_touches() == 1) {
                    emit(EV_KEY, BTN_TOUCH, 1);
                    emit(EV_ABS, ABS_X, x);
                    emit(EV_ABS, ABS_Y, y);
                }
                emit(EV_SYN, SYN_REPORT, 0);
                break;

            case 1: /* UP */
                slot = find_slot(ptr_id);
                if (slot < 0) break;
                emit(EV_ABS, ABS_MT_SLOT, slot);
                emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
                slot_owner[slot] = -1;
                if (active_touches() == 0)
                    emit(EV_KEY, BTN_TOUCH, 0);
                emit(EV_SYN, SYN_REPORT, 0);
                break;

            case 2: /* MOVE */
                slot = find_slot(ptr_id);
                if (slot < 0) break;
                emit(EV_ABS, ABS_MT_SLOT, slot);
                emit(EV_ABS, ABS_MT_POSITION_X, x);
                emit(EV_ABS, ABS_MT_POSITION_Y, y);
                if (slot == 0) {
                    emit(EV_ABS, ABS_X, x);
                    emit(EV_ABS, ABS_Y, y);
                }
                emit(EV_SYN, SYN_REPORT, 0);
                break;
            }
        }

        /* Client disconnected — release all touches */
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (slot_owner[i] != -1) {
                emit(EV_ABS, ABS_MT_SLOT, i);
                emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
                slot_owner[i] = -1;
            }
        }
        emit(EV_KEY, BTN_TOUCH, 0);
        emit(EV_SYN, SYN_REPORT, 0);

        close(client);
        fprintf(stderr, "kaos-touch-input: client disconnected\n");
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return 0;
}
