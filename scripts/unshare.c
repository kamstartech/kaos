/*
 * unshare - static replacement for util-linux unshare
 * Supports all Linux namespace flags including -C (cgroup)
 * Built for Android /system/bin/ deployment
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mount.h>

#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME   0x00000080
#endif

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [PROG [ARGS...]]\n"
        "\n"
        "Run a program with some namespaces unshared from the parent.\n"
        "\n"
        "Options:\n"
        "  -m, --mount       unshare mount namespace\n"
        "  -u, --uts         unshare UTS namespace (hostname)\n"
        "  -i, --ipc         unshare IPC namespace\n"
        "  -n, --net         unshare network namespace\n"
        "  -p, --pid         unshare PID namespace\n"
        "  -U, --user        unshare user namespace\n"
        "  -C, --cgroup      unshare cgroup namespace\n"
        "  -T, --time        unshare time namespace\n"
        "  -f, --fork        fork before exec\n"
        "  --mount-proc[=DIR]  mount /proc after unsharing PID ns\n"
        "  -h, --help        show this help\n",
        prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    int flags = 0;
    int do_fork = 0;
    int mount_proc = 0;
    const char *proc_dir = "/proc";
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }

        if (argv[i][0] != '-')
            break;

        /* Long options */
        if (strcmp(argv[i], "--mount") == 0) { flags |= CLONE_NEWNS; continue; }
        if (strcmp(argv[i], "--uts") == 0)   { flags |= CLONE_NEWUTS; continue; }
        if (strcmp(argv[i], "--ipc") == 0)   { flags |= CLONE_NEWIPC; continue; }
        if (strcmp(argv[i], "--net") == 0)   { flags |= CLONE_NEWNET; continue; }
        if (strcmp(argv[i], "--pid") == 0)   { flags |= CLONE_NEWPID; continue; }
        if (strcmp(argv[i], "--user") == 0)  { flags |= CLONE_NEWUSER; continue; }
        if (strcmp(argv[i], "--cgroup") == 0){ flags |= CLONE_NEWCGROUP; continue; }
        if (strcmp(argv[i], "--time") == 0)  { flags |= CLONE_NEWTIME; continue; }
        if (strcmp(argv[i], "--fork") == 0)  { do_fork = 1; continue; }
        if (strcmp(argv[i], "--help") == 0)  { usage(argv[0]); }
        if (strncmp(argv[i], "--mount-proc", 12) == 0) {
            mount_proc = 1;
            flags |= CLONE_NEWPID;
            if (argv[i][12] == '=')
                proc_dir = &argv[i][13];
            continue;
        }

        /* Short options (can be combined: -mpf) */
        const char *p = &argv[i][1];
        while (*p) {
            switch (*p) {
                case 'm': flags |= CLONE_NEWNS;      break;
                case 'u': flags |= CLONE_NEWUTS;      break;
                case 'i': flags |= CLONE_NEWIPC;      break;
                case 'n': flags |= CLONE_NEWNET;       break;
                case 'p': flags |= CLONE_NEWPID;       break;
                case 'U': flags |= CLONE_NEWUSER;      break;
                case 'C': flags |= CLONE_NEWCGROUP;    break;
                case 'T': flags |= CLONE_NEWTIME;      break;
                case 'f': do_fork = 1;                  break;
                case 'h': usage(argv[0]);               break;
                default:
                    fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], *p);
                    usage(argv[0]);
            }
            p++;
        }
    }

    if (i >= argc) {
        fprintf(stderr, "%s: no program specified\n", argv[0]);
        usage(argv[0]);
    }

    if (unshare(flags) != 0) {
        fprintf(stderr, "%s: unshare(0x%x) failed: %s\n",
                argv[0], flags, strerror(errno));
        return 1;
    }

    /* CLONE_NEWNS alone does not isolate the new mount namespace's future
     * mounts -- propagation type is inherited from whatever the root
     * mount already was (shared, on Android), so anything mounted after
     * this point keeps propagating straight back out to the parent
     * namespace until explicitly marked private. Confirmed live
     * 2026-08-28: every mount kaos-service's ns-entry.sh created inside
     * the "isolated" namespace -- proc, sys, dev, every cgroup
     * controller, devpts, mqueue, debugfs, tracefs, configfs, 32 mounts
     * total -- was visible from Android's own host mount table the whole
     * time. This has to run immediately after unshare() and before any
     * mount the caller goes on to make, and only when a mount namespace
     * was actually requested -- doing this unconditionally would instead
     * make the CALLER's own (unshared) namespace private, which is a much
     * bigger, wrong-by-default change to make when -m was not passed.
     */
    if (flags & CLONE_NEWNS) {
        if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
            fprintf(stderr, "%s: making new mount namespace private failed: %s\n",
                    argv[0], strerror(errno));
        }
    }

    if (do_fork) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "%s: fork failed: %s\n", argv[0], strerror(errno));
            return 1;
        }
        if (pid > 0) {
            /* Parent: wait for child */
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            return 1;
        }
        /* Child continues */
    }

    if (mount_proc) {
        if (mount("none", proc_dir, NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
            /* Ignore error - might not be mounted yet */
        }
        if (mount("proc", proc_dir, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
            fprintf(stderr, "%s: mount proc failed: %s\n", argv[0], strerror(errno));
        }
    }

    execvp(argv[i], &argv[i]);
    fprintf(stderr, "%s: exec %s failed: %s\n", argv[0], argv[i], strerror(errno));
    return 1;
}
