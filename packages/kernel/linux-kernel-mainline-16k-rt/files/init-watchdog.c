#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REAL_ROOT "/dev/nvme0n1p2"

static void force_reboot(void)
{
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        force_reboot();
}

static int open_root_dir(void)
{
    make_dir("/diag-root");
    if (mount(REAL_ROOT, "/diag-root", "ext4", 0, "errors=remount-ro") < 0)
        return -1;
    return open("/diag-root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int openat_dir(int parent, const char *name)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return fd;
}

static int write_fd_file(int dirfd, const char *name, const char *data, size_t len)
{
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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

static int open_home_log_dir(int rootfd)
{
    static const char *parts[] = {"home", "houge", "infra", "kernel", "logs"};
    int fd = fcntl(rootfd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        int next = openat_dir(fd, parts[i]);
        close(fd);
        if (next < 0)
            return -1;
        fd = next;
    }
    return fd;
}

static void capture_and_reboot(int rootfd, const char *phase)
{
    static char kmsg[8 * 1024 * 1024];
    char header[4096];
    struct utsname uts;
    char cmdline[2048] = "";

    int cfd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, cmdline, sizeof(cmdline) - 1);
        if (n > 0)
            cmdline[n] = '\0';
        close(cfd);
    }
    int len = klogctl(3, kmsg, (int)sizeof(kmsg) - 1);
    if (len < 0)
        len = 0;
    kmsg[len] = '\0';

    if (uname(&uts) != 0)
        snprintf(uts.release, sizeof(uts.release), "unknown");

    int hn = snprintf(header, sizeof(header),
                      "WATCHDOG phase=%s release=%s cmdline=%s bytes=%d\n",
                      phase, uts.release, cmdline, len);
    write_fd_file(rootfd, ".kernel-7.3-watchdog.log", header, (size_t)hn);
    write_fd_file(rootfd, ".kernel-7.3-watchdog.dmesg", kmsg, (size_t)len);

    int bootdir = openat_dir(rootfd, "boot");
    if (bootdir >= 0) {
        write_fd_file(bootdir, "kernel-7.3-watchdog.log", header, (size_t)hn);
        write_fd_file(bootdir, "kernel-7.3-watchdog.dmesg", kmsg, (size_t)len);
        close(bootdir);
    }
    int homedir = open_home_log_dir(rootfd);
    if (homedir >= 0) {
        write_fd_file(homedir, "kernel-7.3-watchdog.log", header, (size_t)hn);
        write_fd_file(homedir, "kernel-7.3-watchdog.dmesg", kmsg, (size_t)len);
        close(homedir);
    }
    syncfs(rootfd);
    force_reboot();
}

static void watchdog_child(int rootfd)
{
    prctl(PR_SET_NAME, "k73-watchdog", 0, 0, 0);
    for (int i = 0; i < 60; i++) {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
    }
    capture_and_reboot(rootfd, "60s");
}

int main(void)
{
    signal(SIGALRM, SIG_IGN);
    make_dir("/dev");
    make_dir("/proc");
    make_dir("/sys");
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    int rootfd = open_root_dir();
    if (rootfd < 0)
        force_reboot();

    pid_t p = fork();
    if (p < 0)
        force_reboot();
    if (p == 0) {
        watchdog_child(rootfd);
        _exit(127);
    }
    close(rootfd);

    char *argv[] = {"/usr/lib/systemd/systemd", NULL};
    char *envp[] = {"HOME=/", "TERM=linux", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
    execve(argv[0], argv, envp);
    force_reboot();
}
