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
        "  -R, --root=DIR    become root of the new mount namespace at DIR\n"
        "                    (chdir DIR, MS_MOVE onto \"/\", chroot \".\") instead\n"
        "                    of just chroot(2)ing into a bind mount under the old\n"
        "                    root -- requires -m. A plain chroot() leaves fs->root\n"
        "                    != the mount namespace's own root, which the kernel's\n"
        "                    current_chrooted() then uses to deny every\n"
        "                    unshare(CLONE_NEWUSER) inside, for every process,\n"
        "                    root included (see kernel/user_namespace.c,\n"
        "                    create_user_ns()) -- this is what breaks bubblewrap\n"
        "                    and any systemd PrivateUsers= service inside DIR.\n"
        "  -h, --help        show this help\n",
        prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    int flags = 0;
    int do_fork = 0;
    int mount_proc = 0;
    const char *proc_dir = "/proc";
    const char *new_root = NULL;
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
        if (strncmp(argv[i], "--root=", 7) == 0) { new_root = &argv[i][7]; continue; }

        /* -R takes its argument as a separate argv token (unlike the other
         * short options, which are flags only and can be clustered, e.g.
         * -mpf) -- handle it before the clustered-short-option loop below
         * so "-R DIR" isn't misparsed as the flag cluster "-R" followed by
         * a bare positional DIR. */
        if (strcmp(argv[i], "-R") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -R requires an argument\n", argv[0]);
                usage(argv[0]);
            }
            new_root = argv[++i];
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

    if (new_root && !(flags & CLONE_NEWNS)) {
        fprintf(stderr, "%s: -R/--root requires -m/--mount\n", argv[0]);
        return 1;
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

    /* -R: become the real root of this (already private, per above) mount
     * namespace, not merely chroot(2)ed into a bind mount under the old
     * one. Same chdir -> MS_MOVE -> chroot(".") sequence already proven by
     * hybridos_switch_root() in system/extras/multirom/trampoline/
     * trampoline.c -- unlike that caller, we don't lazy-unmount anything
     * else first: DIR is expected to be a single leaf bind mount (not the
     * real "/" with 100+ Android mounts hanging off it), so MS_MOVEing it
     * onto "/" just re-parents that one mount (and anything already
     * mounted under it) without touching Android's own mount tree, which
     * stays reachable under it (now invisible, still mounted) exactly as
     * it would after a plain chroot(2) -- this only changes what the
     * kernel considers this mount namespace's root to be. Must run before
     * mount_proc below, so --mount-proc=DIR (if ever combined with -R) is
     * relative to the new root, not the old one. */
    if (new_root) {
        if (chdir(new_root) != 0) {
            fprintf(stderr, "%s: chdir '%s' failed: %s\n", argv[0], new_root, strerror(errno));
            return 1;
        }
        if (mount(new_root, "/", NULL, MS_MOVE, NULL) != 0) {
            fprintf(stderr, "%s: MS_MOVE '%s' onto '/' failed: %s\n",
                    argv[0], new_root, strerror(errno));
            return 1;
        }
        if (chroot(".") != 0) {
            fprintf(stderr, "%s: chroot failed: %s\n", argv[0], strerror(errno));
            return 1;
        }
        if (chdir("/") != 0) {
            fprintf(stderr, "%s: chdir '/' after chroot failed: %s\n", argv[0], strerror(errno));
            return 1;
        }
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
