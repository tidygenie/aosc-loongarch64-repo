#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MS_RELATIME
#define MS_RELATIME (1 << 21)
#endif

static void force_reboot(int sig)
{
    (void)sig;
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

static int write_all(const char *path, const char *data, size_t len, int append)
{
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0600);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        force_reboot(0);
}

int main(void)
{
    static char kmsg[8 * 1024 * 1024];
    char prefix[512];
    struct utsname uts;

    signal(SIGALRM, force_reboot);
    alarm(20);

    make_dir("/dev");
    make_dir("/proc");
    make_dir("/sys");
    make_dir("/root");
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    if (uname(&uts) == 0)
        snprintf(prefix, sizeof(prefix),
                 "EARLYLOG release=%s version=%s machine=%s\n",
                 uts.release, uts.version, uts.machine);
    else
        snprintf(prefix, sizeof(prefix), "EARLYLOG uname failed errno=%d\n", errno);

    int cmd_fd = open("/proc/cmdline", O_RDONLY);
    char cmdline[2048] = "";
    if (cmd_fd >= 0) {
        ssize_t n = read(cmd_fd, cmdline, sizeof(cmdline) - 1);
        if (n > 0)
            cmdline[n] = '\0';
        close(cmd_fd);
    }

    int len = klogctl(3, kmsg, (int)sizeof(kmsg) - 1);
    if (len < 0)
        len = 0;
    kmsg[len] = '\0';

    if (mount("/dev/nvme0n1p2", "/root", "ext4", 0,
              "errors=remount-ro") < 0) {
        char fail[512];
        snprintf(fail, sizeof(fail),
                 "%sEARLYLOG root mount failed errno=%d (%s)\ncmdline=%s\n",
                 prefix, errno, strerror(errno), cmdline);
        /* The normal root is unavailable; this boot is still intended to reboot. */
        write_all("/dev/kmsg", fail, strlen(fail), 1);
        force_reboot(0);
    }

    make_dir("/root/boot");
    make_dir("/root/home");
    make_dir("/root/home/houge");
    make_dir("/root/home/houge/infra");
    make_dir("/root/home/houge/infra/kernel");
    make_dir("/root/home/houge/infra/kernel/logs");

    char header[4096];
    int hn = snprintf(header, sizeof(header),
                      "%sEARLYLOG cmdline=%s\nEARLYLOG dmesg_bytes=%d\n",
                      prefix, cmdline, len);
    const char *paths[] = {
        "/root/boot/kernel-7.3-early-debug.log",
        "/root/home/houge/infra/kernel/logs/kernel-7.3-early-debug.log",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        unlink(paths[i]);
        write_all(paths[i], header, (size_t)hn, 0);
        write_all(paths[i], kmsg, (size_t)len, 1);
    }

    sync();
    force_reboot(0);
    return 0;
}
