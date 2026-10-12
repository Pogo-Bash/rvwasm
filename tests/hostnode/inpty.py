# A program in a terminal of its own, driven from the outside:
#   python3 inpty.py SECONDS 'COMMAND' [EXPECT KEYS]...
# runs COMMAND in a pseudo-terminal of 120x40 for at most SECONDS. Whenever
# what the program has shown (since the previous keys) contains EXPECT, with
# the spaces taken out, KEYS are typed (escapes as in Python: \r is Enter;
# the word END instead of keys ends the run there).
# Prints what was shown with the terminal's escape sequences stripped, then
# one line: when the first output came, when each key was typed, when the
# program ended and how.
#
# For programs that want a terminal (Claude Code), on this machine's console
# or anywhere: the machine's own console (hvc0) is fine too, but a program
# under `timeout` is put in a process group of its own and stops on its first
# attempt to change the terminal (SIGTTOU); use `timeout --foreground`.
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

seconds = float(sys.argv[1])
command = sys.argv[2]
steps = list(zip(sys.argv[3::2], sys.argv[4::2]))


def strip(data):
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b[@-Z\\-_]|\r", "", text)
    return re.sub(r"\n{3,}", "\n\n", text)


began = time.time()
pid, fd = pty.fork()
if pid == 0:
    os.execvp("sh", ["sh", "-c", command])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
shown = b""
events = []
first = None
status = None
while time.time() - began < seconds:
    ready, _, _ = select.select([fd], [], [], 0.1)
    if ready:
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        if first is None and data.strip():
            first = time.time() - began
        shown += data
    text = strip(shown).replace(" ", "")
    while steps and steps[0][0] in text:
        expect, keys = steps.pop(0)
        if keys == "END":
            events.append("%s seen at %.1f s" % (expect, time.time() - began))
            steps = None
            break
        keys = keys.encode().decode("unicode_escape").encode("latin1")
        os.write(fd, keys)
        events.append("keys %r at %.1f s" % (keys, time.time() - began))
        shown = b""
        text = ""
    if steps is None:
        break
    done, result = os.waitpid(pid, os.WNOHANG)
    if done:
        status = result
        events.append("ended at %.1f s" % (time.time() - began))
        break
if status is None:
    os.kill(pid, signal.SIGKILL)
    status = "still running"
print(strip(shown)[-6000:])
print("--- first output at %s; %s; status %s; %.1f s in all" % (
    "%.1f s" % first if first else "never", "; ".join(events) or "no keys", status, time.time() - began))
