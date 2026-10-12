// How a native program starts another one on linux-wasm (no fork(): ENOSYS): posix_spawn, vfork, and
// QProcess (Qt's forkfd: clone(CLONE_PIDFD | CLONE_VFORK | CLONE_VM) with a function, else fork()).
//   launch-test [spawn|vfork|qprocess|detached]... (all by default)
#include <QCoreApplication>
#include <QProcess>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <spawn.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <climits>
#include <sys/syscall.h>
#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif

extern char **environ;

static void spawnTest()
{
    pid_t pid = 0;
    char *argv[] = {(char *)"/bin/echo", (char *)"posix_spawn child ran", nullptr};
    int r = posix_spawn(&pid, argv[0], nullptr, nullptr, argv, environ);
    int status = 0;
    if (r == 0)
        waitpid(pid, &status, 0);
    printf("spawn: posix_spawn %d (%s), pid %d, status %d\n", r, strerror(r), pid, status);
}

static void vforkTest()
{
    errno = 0;
    pid_t pid = vfork();
    if (pid == 0) {
        execl("/bin/echo", "echo", "vfork child ran", (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    if (pid > 0)
        waitpid(pid, &status, 0);
    printf("vfork: pid %d errno %d (%s) status %d\n", pid, errno, strerror(errno), status);
}

static void forkTest()
{
    errno = 0;
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    printf("fork: %d errno %d (%s)\n", pid, errno, strerror(errno));
}

static int cloneChild(void *)
{
    execl("/bin/echo", "echo", "clone child ran", (char *)nullptr);
    return 127;
}

// What Qt's forkfd does: P_PIDFD in waitid(), then clone() with a function, CLONE_PIDFD | CLONE_VFORK | CLONE_VM.
static void cloneTest(bool pidfd)
{
    errno = 0;
    int r = syscall(SYS_waitid, P_PIDFD, INT_MAX, nullptr, WEXITED | WNOHANG, nullptr);
    printf("clone: waitid(P_PIDFD, INT_MAX) %d errno %d (%s)\n", r, errno, strerror(errno));
    __attribute__((aligned(64))) static char stack[65536];
    int fd = -1;
    unsigned long flags = (pidfd ? CLONE_PIDFD : 0) | CLONE_VFORK | CLONE_VM | SIGCHLD;
    errno = 0;
    pid_t pid = clone(cloneChild, stack + sizeof stack, flags, nullptr, &fd, nullptr, nullptr);
    int status = 0;
    if (pid > 0)
        waitpid(pid, &status, 0);
    printf("clone%s: pid %d errno %d (%s) pidfd %d status %d\n", pidfd ? "+pidfd" : "", pid, errno, strerror(errno), fd,
           status);
}

static void qprocessTest()
{
    QProcess p;
    p.start(QStringLiteral("/bin/echo"), {QStringLiteral("QProcess child ran")});
    bool started = p.waitForStarted(5000);
    bool finished = p.waitForFinished(5000);
    printf("qprocess: started %d finished %d error %d (%s) exit %d out \"%s\"\n", started, finished, int(p.error()),
           qPrintable(p.errorString()), p.exitCode(), p.readAllStandardOutput().trimmed().constData());
}

static void detachedTest()
{
    qint64 pid = 0;
    bool ok = QProcess::startDetached(QStringLiteral("/bin/echo"), {QStringLiteral("detached child ran")}, QString(), &pid);
    printf("detached: %d pid %lld\n", ok, pid);
    sleep(1);
}

int main(int argc, char **argv)
{
    std::set_terminate([] {
        printf("launch-test: std::terminate (uncaught exception %d)\n", std::uncaught_exceptions());
        fflush(stdout);
        abort();
    });
    static pid_t mainPid = getpid();
    signal(SIGABRT, [](int) {
        char b[128];
        int n = snprintf(b, sizeof b, "launch-test: SIGABRT in pid %d tid %ld (main %d)\n", getpid(), (long)syscall(SYS_gettid), mainPid);
        write(2, b, n);
    });
    QCoreApplication app(argc, argv);
    QStringList what = app.arguments().mid(1);
    if (what.isEmpty())
        what = {QStringLiteral("fork"), QStringLiteral("spawn"), QStringLiteral("vfork"), QStringLiteral("qprocess"),
                QStringLiteral("detached")};
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (const QString &w : what) {
        if (w == QLatin1String("fork"))
            forkTest();
        else if (w == QLatin1String("spawn"))
            spawnTest();
        else if (w == QLatin1String("clone"))
            cloneTest(false);
        else if (w == QLatin1String("clonepidfd"))
            cloneTest(true);
        else if (w == QLatin1String("vfork"))
            vforkTest();
        else if (w == QLatin1String("qprocess"))
            qprocessTest();
        else if (w == QLatin1String("detached"))
            detachedTest();
    }
    printf("launch-test: done\n");
    return 0;
}
