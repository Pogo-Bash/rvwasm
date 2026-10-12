#!/usr/bin/env python3
"""syscall() in the sysroot's <unistd.h> with every argument made a long (build-sysroot.sh runs this).

linux-wasm's musl counts the arguments of syscall(n, ...) with a macro and reads each as a long with
va_arg. Wasm's C varargs keep an int an int (4 bytes, where x86-64 would have widened it in a
register), so syscall(SYS_futex, addr, FUTEX_WAIT, val, ...) gave the kernel op and val packed into
one argument and every later one shifted: Qt's QMutex (its own futex calls) waited for the wrong
value and woke nobody, and a Qt Quick program spun and hung. The macro now casts each argument."""
import sys

path = sys.argv[1]
s = open(path).read()
old = "#define syscall(...) syscall(__syscall_nargs(__VA_ARGS__), __VA_ARGS__)\n"
new = """/* nativeqt (sysroot-syscall.py): every argument a long, as syscall() reads them. */
#define __syscall_long0(n) n
#define __syscall_long1(n,a) n,(long)(a)
#define __syscall_long2(n,a,b) n,(long)(a),(long)(b)
#define __syscall_long3(n,a,b,c) n,(long)(a),(long)(b),(long)(c)
#define __syscall_long4(n,a,b,c,d) n,(long)(a),(long)(b),(long)(c),(long)(d)
#define __syscall_long5(n,a,b,c,d,e) n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e)
#define __syscall_long6(n,a,b,c,d,e,f) n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f)
#define __syscall_long7(n,a,b,c,d,e,f,g) n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f),(long)(g)
#define __syscall_longx(k) __syscall_long##k
#define __syscall_longn(k) __syscall_longx(k)
#define syscall(...) syscall(__syscall_nargs(__VA_ARGS__), __syscall_longn(__syscall_nargs(__VA_ARGS__))(__VA_ARGS__))
"""
if "__syscall_long0" in s:
    print("unistd.h: syscall() already casts")
    sys.exit(0)
assert old in s, "unistd.h: the syscall() macro is not the one expected"
open(path, "w").write(s.replace(old, new))
print("unistd.h: syscall() casts its arguments")
