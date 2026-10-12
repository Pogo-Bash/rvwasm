#!/usr/bin/env python3
"""Our changes to linux-wasm's JavaScript runtime (idempotent).

Usage: patch-runtime.py path/to/linux-wasm/runtime

1. The JIT host hook (linux-worker.js). A program running on Linux/Wasm cannot
   create Wasm code by itself: only the host can compile a module. This gives
   user programs one extra import, __rv_jit_install, which compiles a module
   the program has generated in its memory and places the module's function in
   the program's own function table, where the program can call it through a
   function pointer.

2. Spare runners (linux.js and linux-worker.js). Every task runs in its own
   Web Worker, and creating one costs about 8 ms before the task's first
   instruction: 4 ms for the browser to start the Worker and load its script
   and 4 ms to instantiate vmlinux in it. Neither depends on the task, so a
   few Workers are kept ready with both already done, and a new task is handed
   to one of them. Creating a process drops from about 9 ms to about 1 ms.

3. The network interface (linux.js and linux-worker.js), the host's side of
   drivers/net/wasm_net.c in our kernel patch. linux() takes one more
   argument, the URL of a WebSocket on which every binary message is one
   Ethernet frame (tools/net-relay.py serves that). Frames the kernel sends
   are posted to the main thread, which owns the socket. Frames that arrive
   are put into a ring in shared memory, from which the kernel's receive
   thread, in whichever Worker it runs, takes them when it asks.

4. The display and input (linux.js and linux-worker.js), the host's side of
   drivers/video/fbdev/wasm_fb.c and drivers/input/misc/wasm_input.c.
   linux() takes a canvas as one more argument. The kernel says where in the
   machine's memory the pixels are; the main thread copies them to the
   canvas for every frame the browser draws. Keys, pointer positions and
   buttons on the canvas go into a ring in shared memory, like network
   frames, from which the kernel's input thread takes them.

5. The disk (linux.js and linux-worker.js), the host's side of
   drivers/block/wasm_blk.c. linux() takes one more argument that says where
   the disk's image is. A worker of its own keeps the disk
   (tools/www/disk-worker.js, which says why); a task that needs sectors
   writes its request into shared memory and sleeps until that worker has
   carried it out.

6. A crash says where (linux-worker.js): when a program traps, the log gets
   the WebAssembly stack trace along with the error.

7. One clock for all Workers (linux-worker.js). The kernel's clock is read in
   whichever Worker a task or a CPU runs, as performance.timeOrigin +
   performance.now(). now() stands still while the computer sleeps, and
   timeOrigin is the wall clock's time when the Worker started, so Workers
   started after a sleep were ahead of those started before it by the length
   of the sleep, for good. The kernel adds up the differences between
   readings: every reading in a newer Worker after one in an older Worker
   moved its time forward by the length of the sleep, and after that the
   older Workers (the CPU that takes the interrupts is one) saw no time pass
   for as long again. A machine whose computer had slept for six minutes
   stood still for six minutes at a time, again and again. Now a Worker whose
   clock has come apart from the wall clock follows the wall clock.

8. The book of runners (linux.js). The main thread is the one place that
   sees every task switch, task creation and task release, in the order the
   Workers asked for them: it keeps the last of them (os.switches, read by
   tools/kernel-flags.js when a machine has stopped), and says in the log
   when what it is asked for does not add up: a task switched to that is
   held to be running already, a task switched from that is not, a task
   released or switched to that has no runner. Two things it did not account
   for are put right as well: a task whose last switch was to a new task
   (create_and_run_task) was never marked as stopped, so a dying task that
   went that way kept its Worker, blocked, for good; and a task made at an
   address a runner is still registered for (the kernel reused a dead task's
   memory) silently took the registration over, with that Worker left
   behind. Such a Worker is now ended, and the log says so.

9. Signal handlers one at a time (linux-worker.js). A handler ends with a
   sigreturn system call, and the kernel may have another signal to deliver at
   that call's return (flow 3 of user_mode_tail()). The host ran the new
   handler first, nested in the native frames of the old one, and unwound the
   old one afterwards: with a signal arriving at every return (rv's
   stop-the-world kick comes every 2 ms to a thread that has not parked, and
   a thread on a starved pool CPU takes longer than that to return from
   anything) the native stack grew by one handler per signal, without bound,
   until the browser threw RangeError (seen: "Maximum call stack size
   exceeded at rv.kick_handler"). The old handler's frames are unwound first
   now, and the new signal is delivered where the old one was: the stack
   stays one handler deep. ?signals=old on the page keeps the old order.

10. A crash inside the kernel is not an exception of the program
   (linux-worker.js). When a Worker's Wasm code throws, the host called the
   kernel's raise_exception(), which handles it as the program's fault:
   right when the program threw, wrong when the error (a RangeError most
   of all) came out of the kernel, in the middle of a system call or of a
   task switch, with its locks held and its CPU's flags cleared: the kernel
   then ran a second time in that Worker, next to the task the scheduler had
   woken for the same CPU, and the two fought over the CPU's flags until a
   task entering the kernel found them in kernel mode ("Syscall called when
   in kernel mode"). Now the host asks the kernel (wasm_cpuflags_now()) and,
   if the CPU was not in user mode, says so in the log and stops this
   Worker instead.
11. JavaScript as a program (linux-worker.js), for hostnode/ (which says what
   it is for: Node.js programs run by the browser's own JavaScript engine). A
   task is a Worker, and what makes system calls in it does not have to be
   Wasm. User programs get one more import, __run_js: the program hands over
   JavaScript text from its memory and stops being run, as it does on exec;
   the text is then run as what this Worker does from now on, with what it
   needs to be a process: the machine's memory, the kernel's system call
   entries and the program's own instance (its C library, its signal
   handlers). The instance stays the task's user program, so signals arrive
   in it as before, at the return of a system call the JavaScript made.
   And what the kernel's callbacks need of the browser (the clock, random
   numbers) is held on to when the Worker starts: such a program shares the
   global object with this script and may replace what is there.
12. Task switches without the main thread (linux-worker.js, linux.js). A
   switch from one task to another on a CPU is a hand-over between their
   Workers: the one that stops wakes the next and waits to be woken itself.
   That went through the main thread (a message to it, and it woke the next
   Worker), the thread that also draws the page and takes the person's
   input, so every switch waited until the page had nothing else to do. Now
   the Worker that stops wakes the next one itself, on words of the next
   task's thread_info in the machine's memory (the kernel's
   wasm_task_switch_area(), kernel patch), and the main thread is told only
   what it needs for ending Workers: a dead task's last switch (task_done).
   The switches go into a ring all Workers share (os.switches reads it with
   the main thread's own notes). ?switches=main on the page (or
   globalThis.rv_query in tools/node-machine.js) keeps the old way, as does a
   kernel without wasm_task_switch_area.

13. Doorbells (linux.js, linux-worker.js). The input and network rings were
   looked at by kernel threads every few milliseconds. The main thread now
   raises an interrupt of the kernel's IRQ_CPU when it puts something into
   either ring (WASM_IRQ_INPUT 2, WASM_IRQ_NET 3: the bit in the word the
   kernel's wasm_irq_cpu_word() names, and a notify on it, as the kernel's
   raise_interrupt() does), and the kernel takes it at once. An input event
   of kind 5 is a doorbell for programs (the kernel's "Wasm host" input
   device, /dev/input/event2): the page rings it when it has written
   something the window server is to see (wlserver/README.md).
14. Waits that a stale wake cannot end (linux-worker.js, linux.js). A wake is
   two steps, a store and a notify, and the thread that does them can be put
   aside by the system between the two: the waiter may then see the store,
   go on without waiting, come back to wait for the NEXT wake, and be woken
   by the late notify of the old one. Atomics.wait then says "ok" with
   nothing changed. linux-wasm's lock_wait() went on regardless: the task ran
   although nothing had switched to it, as a CPU another task was already
   running as ("Not the current task (switched to)", seen when Firefox
   starts; reproduced under node-machine with sched_bench storm and a log of
   every wait that returned with the lock still 0). The disk's request had
   the same gap (a task read the result of a request the disk's worker had
   not done yet), as did the console's read. Each wait now looks at the
   word it waits on, and waits again until it has really changed (for the
   switch lock: takes the wake with an exchange). tools/www/disk-worker.js
   likewise serves a request only when one is there.

15. A memory as large as the machine's (linux.js). The memory was made
   with a maximum of 0x10000 pages, 4 GiB, though it is a 64-bit one: the
   kernel could not be given more than that whatever mem= said. The maximum
   is now 0x40000 pages, 16 GiB: the most V8 lets a 64-bit memory have
   (kV8MaxWasmMemory64Pages, src/wasm/wasm-limits.h; the JavaScript API
   refuses a larger maximum outright) and the maximum every program on the
   machine is linked with (--max-memory=1<<34), which a memory it imports
   may not exceed. A shared memory's maximum is reserved address space,
   not memory: what the machine has is what the kernel grows it to (mem=,
   tools/www/dev.html). Addresses above 4 GiB then reach the host as well:
   the doorbell (13) keeps the kernel's word in 64 bits, and a disk request
   (5) its buffer's address in two words.

16. A bound on translated code (linux.js, linux-worker.js). The browser
   gives all the page's WebAssembly code together a limit (V8's committed
   code space, 4 GiB) and ends the tab with "Aw, Snap!" when it is reached;
   rv makes a module of every region it translates, and every module, however
   small, takes at least a page of it (16 KiB on a Mac). The JIT hook keeps
   account: an estimate of what each module of a Worker's program takes
   (3.5 times its length plus 2 KiB, in pages of 16 KiB), summed per
   Worker in a cell of a
   SharedArrayBuffer every Worker has (jit_code: [0] the budget in KiB, [1]
   the sum of all cells, then the cells), the cell zeroed when the Worker is
   ended or its program replaced by exec. __rv_jit_install with a length of 0
   empties a slot of the table (the program has let go of what was there, and
   the engine can collect it), and with a slot of -2 as well it answers the
   budget and the sum (budget << 32 | sum, in KiB), so rv can keep within the
   budget (rv/jit.c, "Room for translated code"). With a slot of -16 - n the
   module is a batch of n regions, which puts its functions into their slots
   itself (element segments); the n slots follow the module in memory, and
   each is accounted a share. ?jitcode=MB on the page
   (globalThis.rv_query under node-machine) sets the budget; 1536 MiB if not.
   (At 512 MiB Chromium's renderer, loading a page with 9 MB of script, was
   at the budget throughout and let go of and translated again 2.4 GiB worth
   of regions in 80 s; the page took twice as long. What V8 had committed
   for wasm code, measured from outside, stayed near 1.1 GiB at every budget
   tried, 512 to 2048: what is let go of and not yet collected comes on top
   of the account, so the budget stays well below V8's 4 GiB.)

17. The console's input without the main thread (linux.js, linux-worker.js).
   The kernel's console driver (hvc_console.c) asks for input every 10 ms
   to 2 s, with its lock held and interrupts off on its CPU. The host
   answered by a message to the main thread, and the asking task waited,
   with that CPU, until the main thread had read the message and answered:
   whenever the page was busy drawing, or the browser ran it slowly because
   it was not in front (a hidden tab's process gets a low priority), a CPU of
   the machine stood still and every writer to the console waited for the
   lock. Typed text now goes into a ring in shared memory (1 MiB; what does
   not fit waits on the main thread, which is asked for more when the ring
   has been half full), and the driver takes it from there at once.
18. Signals looked at before their handler is called (linux-worker.js).
   A handler is called through the program's function table, by the index
   the kernel put in the signal's frame; an index that is no function there,
   or one of another type, ends the program with "null function or function
   signature mismatch" in __libc_handle_signal, and nothing more is known.
   Now, when that happens, the log says the signal, whether SA_SIGINFO, the
   index, what the table has there (nothing, or a function with how many
   parameters: a handler has 1 or 3, a translated region of rv's 3), the
   table's length, the frame's address, and which program this Worker runs:
   the how-many-th it loaded (an exec), its size, whether it is the emulator,
   and whether the Worker began as a spare runner; the program then ends as
   before. And the reason it happened: musl's __libc_handle_signal (in every
   native program) reads the frame's info_param not from the frame but from
   address 12 of memory (a stray constant in src/signal/wasm/restore.S,
   fixed in patches/musl-wasm64.diff; programs need rebuilding with it).
   While that word is 0 a handler without SA_SIGINFO is called right; once
   anything on the machine has written there (a store through a null
   pointer), every handler of every program is called as one with
   SA_SIGINFO, and fails. Until the programs are rebuilt the runtime reads
   the frame itself, and when musl would read it wrong (that word not 0, or
   a handler with SA_SIGINFO, which musl never called right) calls the
   handler itself and then makes rt_sigreturn as musl's __restore_rt does;
   and it says once, in the log, that the first bytes of memory were
   written. (Only for a program that has that read: one linked with the
   fixed musl delivers its own signals; the runtime looks for the read's
   code when the program is loaded.) And __rv_jit_install never puts
   translated code into, or empties, a slot of the program's own functions
   (those the table had when the program was instantiated; translated code
   only goes into slots the table grew by): it says so and refuses.
   ?sigfix=0 on the page turns what 18 and 19 do off, leaving what they log.
19. A new thread's stack aligned to 16 bytes (linux-worker.js). musl's
   pthread_create() hands clone a stack top that is only 8-byte aligned (the
   top of the stack, below the thread's TLS, less its start arguments), and
   the wasm _Clone (src/thread/wasm/clone.S) does not align it as other
   architectures' clone does; so every thread a native program started ran
   with its stack pointer 8 bytes off. The code clang makes takes the stack
   to be 16-byte aligned (an address in a frame's array is made with an or,
   not an add), and such code wrote into the wrong places: in rv's threads
   the qsort() of a hint (musl's smoothsort) put a block's address over its
   array of pointers and then copied through a null pointer, into the first
   bytes of memory, which every program's __libc_handle_signal read
   SA_SIGINFO from (18). Here the child's stack is moved down to the next
   16-byte boundary before __libc_clone_callback() runs: the two words it
   takes from there (the function and its argument) are moved with it, and
   what it leaves is 16-byte aligned. (patches/musl-wasm64.diff aligns it in
   _Clone as well, for programs linked again.)
20. A trap of the program runs its handler (linux-worker.js). Every trap of a
   program's code (an access out of bounds, a division by zero, unreachable)
   went to the kernel's raise_exception(), which made it SIGILL; with a
   handler for it (crash reporters have one: KCrash in every KDE program,
   Firefox, the JVM) the kernel set up the handler's frame and nothing ran
   it, and the task hung for good, unkillable, its parent waiting. Now the
   host asks the kernel's wasm_user_fault (patches/kernel-lwfix.diff) with
   the program's stack pointer and the signal the trap means: SIGSEGV for an
   access out of bounds, a call through a bad function pointer or a stack
   that ran out, SIGFPE for a division by zero or an integer conversion out
   of range, SIGILL for the rest. The handler runs on the way out, as at the
   return of a system call; it may exit, or exec. A trap cannot be resumed,
   so a handler that returns is followed by death by the signal (Linux would
   run the trapping instruction again, and take the trap again); so is a
   trap without a handler, or with the signal blocked. A trap in the handler
   is a trap again (its own signal is blocked there, so that one is fatal).
   A kernel without wasm_user_fault gets raise_exception() as before.
21. Ending a program that never comes into the kernel (linux.js,
   linux-worker.js). A task's program runs in its Worker until it makes a
   system call, and only there does the kernel act on signals: SIGKILL did
   not end a program in a loop, and an exit_group() or exec() of one of its
   threads waited for good for another that computed. Now the kernel
   (traps.c, wasm_kick_user_task) asks the host to end the Worker of a task
   that has a fatal signal and is running its program (wasm_release_task
   with the task's address + 1, an odd address no task has: older runtimes
   say there is no such task and go on); the main thread does it if the
   task's state word (wasm_task_user_state) still says so (it changes KILL
   to ENDING; anything else means the task came into the kernel by itself),
   and starts a Worker that runs the task's exit (wasm_user_task_end), as the
   task. Non-fatal signals still wait for the program's next system call.
   With a kernel without kernel-lwfix.diff nothing of this happens.
22. A signal at a new task's first return to its program (linux-worker.js).
   A clone()'s child that had a signal sent to it before it first ran (a
   SIGCHLD, a Ctrl-C to its group) was asked to run the handler before its
   program was instantiated: a TypeError, and the child died with SIGILL.
   The handler now runs when the program is there, before the clone's
   function.
23. A function table as large as the program asks for (linux-worker.js). The
   table every program gets had 4096 slots; a program whose import of the
   table asks for more (a C++ program with Qt in it: tens of thousands of
   functions have their address taken) could not be instantiated. The import
   section of the program is read when it is loaded, and its table gets at
   least the size it asks for.
24. A thread does not apply the program's data relocations again
   (linux-worker.js). Every instantiation called __wasm_apply_data_relocs,
   which writes the initial value of every pointer in the program's
   initialised data; a thread is the program instantiated again over the
   same memory, so each new thread put back every such pointer the program
   had changed since it started (a Qt Quick program hung at random after
   its QML loader and render threads started). Only the instantiation that
   starts a program (exec) applies them now.
25. Sound (linux.js, linux-worker.js), the host's side of
   sound/drivers/wasm_audio.c (patches/kernel-audio.diff). The kernel keeps
   a header in its memory that says, for each playback substream, where its
   ring is (the substream's DMA buffer), its format, and how far the host
   has played it; the kernel exports where the header is
   (wasm_audio_header()). A Worker asks, as it tells the main thread the
   interrupt word (13), and linux() gives both to the page as os.audio
   (header(), irq_word(): 0 until known, and for good with a kernel without
   the card). The page's player (tools/www/audio.js, audio-worklet.js) does
   the rest: nothing here plays anything.
"""
import sys

HOOK = """
            // JIT support (rv/jit.c): compile the Wasm module the program built at
            // [ptr, ptr + len) and put its exported function "r" into the program's
            // function table, at `slot` or, if that is negative, in a new slot.
            // Function imports of the module (namespace "rv") are resolved against
            // the program's own exports, so generated code can call back into it.
            // Returns the slot, or -1 if the module did not compile.
            __rv_jit_install: (ptr, len, slot) => {
              try {
                const start = Number(ptr);
                // A copy: a module cannot be compiled from shared memory.
                const bytes = new Uint8Array(memory.buffer).slice(start, start + Number(len));
                const module = new WebAssembly.Module(bytes);
                // The table: regions jump to one another through it (tail calls).
                const imports = {
                  env: { memory: memory, table: user_executable_imports.env.__indirect_function_table },
                  rv: {},
                };
                for (const item of WebAssembly.Module.imports(module)) {
                  if (item.module == "rv") {
                    imports.rv[item.name] = user_executable_instance.exports[item.name];
                  }
                }
                const instance = new WebAssembly.Instance(module, imports);
                const table = user_executable_imports.env.__indirect_function_table;
                let index = Number(slot);
                if (index < 0) {
                  index = table.grow(1);
                }
                table.set(index, instance.exports.r);
                return Ulong(index);
              } catch (error) {
                console.error("__rv_jit_install: " + error);
                return Ulong(-1);
              }
            },
"""

ANCHOR = "            __wasm_abort: () => {\n"



SPARE_WORKER_ANCHOR = "      vmlinux_setup().then(vmlinux_run).catch(wasm_error).then(user_executable_chain);\n"
SPARE_WORKER = """      if (message.runner_type == "spare") {
        // A spare runner (see linux.js): what does not depend on the task is done ahead of time, which is loading
        // this script and instantiating vmlinux. The task itself arrives later, in an "assign" message.
        const ready = vmlinux_setup();
        message_callbacks.assign = (assignment) => {
          message = assignment;
          runner_name = message.runner_name;
          if (message.user_executable) {
            host_callbacks.wasm_load_executable(
              message.user_executable.bin_start,
              message.user_executable.bin_end,
              message.user_executable.data_start,
              message.user_executable.table_start);
          }
          ready.then(vmlinux_run).catch(wasm_error).then(user_executable_chain);
        };
        return;
      }

"""

SPARE_MAIN_OLD = """    tasks[new_task] = make_vmlinux_runner(name + " (" + new_task + ")", options);
  };
"""
SPARE_MAIN_NEW = """    const runner_name = name + " (" + new_task + ")";

    // Use a spare runner if there is one: it has started and instantiated vmlinux already.
    const spare = spare_runners.shift();
    if (spare) {
      spare.worker.postMessage({ ...options, method: "assign", runner_name: runner_name });
      tasks[new_task] = spare;
    } else {
      tasks[new_task] = make_vmlinux_runner(runner_name, options);
    }

    // Replace it (after the hand-over above: this is not on the new task's critical path).
    while (spare_runners.length < SPARE_RUNNERS) {
      spare_runners.push(make_vmlinux_runner("spare", { runner_type: "spare" }));
    }
  };
"""
SPARE_MAIN_DECL_ANCHOR = "  /**\n   * Create and run one task. This task has been switch_to():ed by the scheduler for the first time.\n"
SPARE_MAIN_DECL = """  /// Runners kept ready for the next tasks: Workers that have loaded their script and instantiated vmlinux, which
  /// together is most of the time it takes to create a task. Filled when the first task is created.
  const spare_runners = [];
  const SPARE_RUNNERS = 8;

"""


NET_WORKER_DECL_ANCHOR = "  let locks = null;\n"
NET_WORKER_DECL = """  let locks = null;

  /// The ring of received network frames, shared with the main thread (see linux.js), or null.
  let net = null;
"""
NET_WORKER_INIT_ANCHOR = "      locks = message.locks;\n"
NET_WORKER_INIT = """      locks = message.locks;
      net = message.net || null;
"""
NET_WORKER_ANCHOR = "    // Host callbacks used by the Wasm-default console driver.\n"
NET_WORKER = """    // Host callbacks used by the network interface (drivers/net/wasm_net.c).

    /// Send one Ethernet frame: the main thread has the connection.
    wasm_driver_net_tx: (frame, count) => {
      port.postMessage({
        method: "net_tx",
        frame: new Uint8Array(memory.buffer).slice(frame, frame + count),
      });
      return count;
    },

    /// Copy the next received frame into the kernel's buffer and return its length, or 0 if there is none.
    wasm_driver_net_rx: (buffer, size) => {
      if (!net) {
        return 0;
      }
      const read = Atomics.load(net.index, 1);
      if (read == Atomics.load(net.index, 0)) {
        return 0;
      }
      const slot = (read % net.slots) * net.slot_size;
      const length = net.bytes[slot] | (net.bytes[slot + 1] << 8);
      const fits = length <= size;
      if (fits) {
        new Uint8Array(memory.buffer).set(net.bytes.subarray(slot + 2, slot + 2 + length), buffer);
      }
      Atomics.store(net.index, 1, (read + 1) | 0);
      return fits ? length : 0;
    },

"""

NET_MAIN_SIGNATURE_OLD = "const linux = async (worker_url, variant, vmlinux, boot_cmdline, initrd, log, console_write) => {\n"
NET_MAIN_SIGNATURE_NEW = "const linux = async (worker_url, variant, vmlinux, boot_cmdline, initrd, log, console_write, net_url) => {\n"
NET_MAIN_SETUP_ANCHOR = "  const text_encoder = new TextEncoder();\n"
NET_MAIN_SETUP = """  const text_encoder = new TextEncoder();

  /// The network (drivers/net/wasm_net.c): every binary message on this WebSocket is one Ethernet frame. Frames
  /// that arrive wait in a ring in shared memory until the kernel's receive thread takes them: index[0] counts the
  /// frames put in, index[1] the frames taken out, and each slot holds a 16-bit length and the frame.
  const net = {
    slots: 256,
    slot_size: 2048,
    index: new Int32Array(new SharedArrayBuffer(8)),
  };
  net.bytes = new Uint8Array(new SharedArrayBuffer(net.slots * net.slot_size));
  let net_socket = null;
  if (net_url) {
    net_socket = new WebSocket(net_url);
    net_socket.binaryType = "arraybuffer";
    net_socket.onopen = () => log("[Main]: network relay connected: " + net_url);
    net_socket.onerror = () => log("[Main]: no network relay at " + net_url);
    net_socket.onmessage = (event) => {
      const frame = new Uint8Array(event.data);
      const written = Atomics.load(net.index, 0);
      if (frame.length > net.slot_size - 2 || ((written - Atomics.load(net.index, 1)) | 0) >= net.slots) {
        return;  // too large, or the kernel is not keeping up: dropped, as a network may
      }
      const slot = (written % net.slots) * net.slot_size;
      net.bytes[slot] = frame.length & 0xff;
      net.bytes[slot + 1] = frame.length >> 8;
      net.bytes.set(frame, slot + 2);
      Atomics.store(net.index, 0, (written + 1) | 0);
    };
    // A connection that ends (the relay restarted, the computer slept, the wifi went away for a while) is made
    // again, with the same address (it is in the URL), every few seconds for as long as it takes: the kernel never
    // notices, and connections in the guest carry on or time out as they would on a network that went away.
    const net_reconnect = (delay) => {
      setTimeout(() => {
        const socket = new WebSocket(net_url);
        socket.binaryType = "arraybuffer";
        socket.onmessage = net_socket.onmessage;
        socket.onopen = () => {
          log("[Main]: network relay connected again: " + net_url);
          net_socket = socket;
        };
        socket.onclose = () => {
          if (net_socket !== socket) {
            net_reconnect(Math.min(delay * 2, 30000));
          }
        };
        socket.addEventListener("close", () => {
          if (net_socket === socket) {
            log("[Main]: network relay lost; connecting again");
            net_reconnect(2000);
          }
        });
      }, delay);
    };
    net_socket.onclose = () => {
      log("[Main]: network relay lost; connecting again");
      net_reconnect(2000);
    };
  }
"""
NET_MAIN_CALLBACK_ANCHOR = "    console_write: (message) => {\n"
NET_MAIN_CALLBACK = """    net_tx: (message) => {
      if (net_socket && net_socket.readyState == WebSocket.OPEN) {
        net_socket.send(message.frame);
      }
    },

    console_write: (message) => {
"""
NET_MAIN_INIT_ANCHOR = "      locks: locks,\n"
NET_MAIN_INIT = """      locks: locks,
      net: net,
"""


GUI_WORKER_DECL_ANCHOR = "  /// The ring of received network frames"
GUI_WORKER_DECL = """  /// The ring of input events, shared with the main thread (see linux.js), or null.
  let input = null;

"""
GUI_WORKER_INIT_ANCHOR = "      net = message.net || null;\n"
GUI_WORKER_INIT = """      net = message.net || null;
      input = message.input || null;
"""
GUI_WORKER_ANCHOR = "    // Host callbacks used by the network interface (drivers/net/wasm_net.c).\n"
GUI_WORKER = """    // Host callbacks used by the display (drivers/video/fbdev/wasm_fb.c).

    /// The pixels to show are at this address from now on: the main thread does the showing.
    wasm_driver_fb_show: (pixels, width, height, stride) => {
      port.postMessage({ method: "fb_show", pixels: pixels, width: width, height: height, stride: stride });
    },

    // Host callbacks used by the keyboard and pointer (drivers/input/misc/wasm_input.c).

    /// Copy up to max waiting events (three 32-bit words each) into the kernel's buffer and return how many.
    wasm_driver_input_rx: (events, max) => {
      if (!input) {
        return 0;
      }
      const out = new Int32Array(memory.buffer, events, max * 3);
      const written = Atomics.load(input.index, 0);
      let read = Atomics.load(input.index, 1);
      let count = 0;
      while (count < max && read != written) {
        const slot = (read % input.slots) * 3;
        out[count * 3] = input.words[slot];
        out[count * 3 + 1] = input.words[slot + 1];
        out[count * 3 + 2] = input.words[slot + 2];
        read = (read + 1) | 0;
        count++;
      }
      Atomics.store(input.index, 1, read);
      return count;
    },

"""

GUI_MAIN_SIGNATURE_OLD = "initrd, log, console_write, net_url) => {\n"
GUI_MAIN_SIGNATURE_NEW = "initrd, log, console_write, net_url, canvas) => {\n"
GUI_MAIN_SETUP_ANCHOR = "  /// The network (drivers/net/wasm_net.c):"
GUI_MAIN_SETUP = """  /// Input (drivers/input/misc/wasm_input.c): events wait in a ring in shared memory, three words each (kind, code,
  /// value), until the kernel's input thread takes them. index[0] counts events put in, index[1] events taken out.
  const input = {
    slots: 1024,
    index: new Int32Array(new SharedArrayBuffer(8)),
  };
  input.words = new Int32Array(new SharedArrayBuffer(input.slots * 3 * 4));
  const INPUT_KEY = 0, INPUT_POSITION = 1, INPUT_BUTTON = 2, INPUT_WHEEL = 3;
  const input_event = (kind, code, value) => {
    const written = Atomics.load(input.index, 0);
    if (((written - Atomics.load(input.index, 1)) | 0) >= input.slots) {
      return;  // nobody is taking them: dropped
    }
    const slot = (written % input.slots) * 3;
    input.words[slot] = kind;
    input.words[slot + 1] = code;
    input.words[slot + 2] = value;
    Atomics.store(input.index, 0, (written + 1) | 0);
  };

  /// What the kernel calls the keys (linux/input-event-codes.h), by the browser's names for the physical keys.
  const key_codes = {
    Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10,
    Digit0: 11, Minus: 12, Equal: 13, Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20,
    KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29,
    KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39,
    Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48,
    KeyN: 49, KeyM: 50, Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56,
    Space: 57, CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68,
    NumLock: 69, ScrollLock: 70, Numpad7: 71, Numpad8: 72, Numpad9: 73, NumpadSubtract: 74, Numpad4: 75,
    Numpad5: 76, Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81, Numpad0: 82,
    NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88, NumpadEnter: 96, ControlRight: 97, NumpadDivide: 98,
    PrintScreen: 99, AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105, ArrowRight: 106,
    End: 107, ArrowDown: 108, PageDown: 109, Insert: 110, Delete: 111, Pause: 119, MetaLeft: 125, MetaRight: 126,
    ContextMenu: 127,
  };
  const button_codes = [0x110, 0x112, 0x111];  // BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, by the browser's button numbers

  /// The display (drivers/video/fbdev/wasm_fb.c): where the kernel last said the pixels are, or null.
  let fb = null;
  if (canvas) {
    const context = canvas.getContext("2d");
    let image = null, image_words = null;
    const draw = () => {
      if (fb) {
        if (!image || canvas.width != fb.width || canvas.height != fb.height) {
          canvas.width = fb.width;
          canvas.height = fb.height;
          image = context.createImageData(fb.width, fb.height);
          image_words = new Uint32Array(image.data.buffer);
        }
        // (A new view every time: the memory's buffer is another object after the memory has grown.)
        if (fb.pixels + fb.height * fb.stride <= memory.buffer.byteLength && fb.stride == fb.width * 4) {
          image.data.set(new Uint8Array(memory.buffer, fb.pixels, fb.height * fb.stride));
          // The fourth byte of a pixel is not used by the kernel; to a canvas it is opacity.
          for (let i = 0; i < image_words.length; i++) {
            image_words[i] |= 0xff000000;
          }
          context.putImageData(image, 0, 0);
        }
      }
      requestAnimationFrame(draw);
    };
    requestAnimationFrame(draw);

    const position = (event) => {
      const box = canvas.getBoundingClientRect();
      const x = Math.max(0, Math.min(32767, Math.round((event.clientX - box.left) / box.width * 32767)));
      const y = Math.max(0, Math.min(32767, Math.round((event.clientY - box.top) / box.height * 32767)));
      input_event(INPUT_POSITION, x, y);
    };
    canvas.addEventListener("keydown", (event) => {
      if (event.code in key_codes) {
        if (!event.repeat) {  // the kernel repeats keys itself
          input_event(INPUT_KEY, key_codes[event.code], 1);
        }
        event.preventDefault();
      }
    });
    canvas.addEventListener("keyup", (event) => {
      if (event.code in key_codes) {
        input_event(INPUT_KEY, key_codes[event.code], 0);
        event.preventDefault();
      }
    });
    canvas.addEventListener("mousemove", position);
    canvas.addEventListener("mousedown", (event) => {
      canvas.focus();
      position(event);
      input_event(INPUT_BUTTON, button_codes[event.button] || 0x110, 1);
      event.preventDefault();
    });
    canvas.addEventListener("mouseup", (event) => {
      input_event(INPUT_BUTTON, button_codes[event.button] || 0x110, 0);
      event.preventDefault();
    });
    canvas.addEventListener("wheel", (event) => {
      input_event(INPUT_WHEEL, 0, event.deltaY < 0 ? 1 : -1);
      event.preventDefault();
    }, { passive: false });
    canvas.addEventListener("contextmenu", (event) => event.preventDefault());
  }

"""
GUI_MAIN_CALLBACK_ANCHOR = "    net_tx: (message) => {\n"
GUI_MAIN_CALLBACK = """    fb_show: (message) => {
      fb = { pixels: message.pixels, width: message.width, height: message.height, stride: message.stride };
    },

"""
GUI_MAIN_INIT_ANCHOR = "      net: net,\n"
GUI_MAIN_INIT = """      net: net,
      input: input,
"""
GUI_MAIN_RETURN_ANCHOR = "  return {\n    key_input: (data) => {\n"
GUI_MAIN_RETURN = """  return {
    /// For looking at a machine that has stopped answering (tools/desktop-drive.js): its memory, and the words
    /// each disk shares with its worker.
    memory: memory,
    disks: disks,

    /// An input event as the canvas would make it: (0, key code, 1 or 0), (1, x, y) with both in 0..32767,
    /// (2, button code, 1 or 0), (3, 0, wheel clicks).
    screen_input: input_event,

    key_input: (data) => {
"""


DISK_WORKER_DECL_ANCHOR = "  /// The ring of input events"
DISK_WORKER_DECL = """  /// For each disk, the words shared with its worker (see disk-worker.js), or null if there is no such disk.
  let disks = [];

"""
DISK_WORKER_INIT_ANCHOR = "      input = message.input || null;\n"
DISK_WORKER_INIT = """      input = message.input || null;
      disks = message.disks || [];
"""
DISK_WORKER_ANCHOR = "    // Host callbacks used by the display (drivers/video/fbdev/wasm_fb.c).\n"
DISK_WORKER = """    // Host callbacks used by the disk (drivers/block/wasm_blk.c).

    /// The size of disk number index in 512-byte sectors, or 0 if there is none.
    wasm_driver_blk_sectors: (index) => {
      const disk = disks[index];
      return disk ? (disk[9] >>> 0) + disk[10] * 4294967296 : 0;
    },

    /// Read (op 0) or write (op 1) sectors from or to the kernel's memory at buffer, or flush (op 2); which disk
    /// is in the bits of op from 8 up. The disk's worker does it (disk-worker.js, which has the meaning of the
    /// shared words); this task sleeps meanwhile.
    wasm_driver_blk_rw: (sector, buffer, count, op) => {
      const disk = disks[op >>> 8];
      if (!disk) {
        return -5;  // EIO
      }
      op &= 0xff;
      // One request at a time: word 0 is the lock.
      while (Atomics.compareExchange(disk, 0, 0, 1) != 0) {
        Atomics.wait(disk, 0, 1);
      }
      disk[3] = op;
      disk[4] = sector % 4294967296 | 0;
      disk[5] = Math.floor(sector / 4294967296);
      disk[6] = buffer % 4294967296 | 0;
      disk[11] = Math.floor(buffer / 4294967296);
      disk[7] = count;
      Atomics.store(disk, 2, 0);
      Atomics.store(disk, 1, 1);
      Atomics.notify(disk, 1);
      // (Until it is done: a late notify of an earlier request can end a wait early, see 14.)
      while (Atomics.load(disk, 2) == 0) {
        Atomics.wait(disk, 2, 0);
      }
      const result = disk[8];
      Atomics.store(disk, 0, 0);
      Atomics.notify(disk, 0, 1);
      return result;
    },

"""

DISK_MAIN_SIGNATURE_OLD = "initrd, log, console_write, net_url, canvas) => {\n"
DISK_MAIN_SIGNATURE_NEW = "initrd, log, console_write, net_url, canvas, disk_options) => {\n"
DISK_MAIN_SETUP_ANCHOR = "  // Create the primary cpu, it will later on callback to us and we start secondaries.\n"
DISK_MAIN_SETUP = """  // The disks (drivers/block/wasm_blk.c), if the page wants any: disk_options is one of these, or a list of
  // them (wda, wdb, ...): { worker: the URL of disk-worker.js, url: where the pieces of the disk's image are,
  // name: what to call it in the origin's private file system, reset: forget what is kept there, keep: the
  // disk is the image only the first time, and its own from then on }. A worker of its own keeps each; the
  // kernel starts when they are ready.
  await Promise.all([].concat(disk_options || []).map((options, index) => new Promise((resolve) => {
    const label = "[Disk " + options.name + "]: ";
    const control = new Int32Array(new SharedArrayBuffer(64));
    const disk_worker = new Worker(options.worker, { name: "disk " + options.name });
    disks[index] = null;
    disk_worker.onmessage = (event) => {
      if (event.data.method == "log") {
        log(label + event.data.text);
      } else {
        if (event.data.method == "ready") {
          disks[index] = control;
        } else {
          log(label + "no disk (" + event.data.text + ")");
        }
        resolve();
      }
    };
    disk_worker.onerror = (error) => {
      log(label + "no disk (" + error.message + ")");
      resolve();
    };
    disk_worker.postMessage({
      memory: memory,
      control: control,
      url: options.url,
      keep: options.keep,
      name: options.name,
      reset: options.reset,
    });
  })));

"""
DISK_MAIN_DECL_ANCHOR = "  /// Memory shared between all CPUs.\n"
DISK_MAIN_DECL = """  /// For each disk, the words shared with its worker (see disk-worker.js), or null if there is no such disk.
  const disks = [];

"""
DISK_MAIN_INIT_ANCHOR = "      input: input,\n"
DISK_MAIN_INIT = """      input: input,
      disks: disks,
"""


CRASH_WORKER_OLD = """        log("Wasm crash: " + error.toString());
"""
CRASH_WORKER_NEW = """        // With where it happened: the functions of a program have names (its "name" section).
        log("Wasm crash: " + error.toString() + "\\n" + String(error.stack || "").split("\\n").slice(0, 14).join("\\n"));
"""


CLOCK_WORKER_OLD = """    wasm_cpu_clock_get_monotonic: () => {
      // Convert this double in ms to u64 in us.
      // Modern browsers can on good days reach 5us accuracy, given that the platform supports it.
      return BigInt(Math.round(1000 * (performance.timeOrigin + performance.now()))) * 1000n;
    },
"""
CLOCK_WORKER_NEW = """    wasm_cpu_clock_get_monotonic: (() => {
      // Every Worker has to tell the same time: the kernel has one clock and reads it wherever a task or a CPU
      // runs. timeOrigin + now() alone does not: now() stands still while the computer sleeps and timeOrigin is
      // the wall clock when the Worker started, so a Worker started after a sleep is ahead of one started before
      // it by the length of the sleep (and one started after the wall clock was set is off by that much). A
      // Worker whose clock has come apart from the wall clock, by more than rounding and the slow corrections a
      // wall clock gets can explain, follows the wall clock from then on: all of them do, so they agree again.
      let correction = 0;
      return () => {
        const now = performance.timeOrigin + performance.now();
        const apart = Date.now() - (now + correction);
        if (apart > 250 || apart < -250) {
          correction += apart;
        }
        // Convert this double in ms to u64 in us.
        // Modern browsers can on good days reach 5us accuracy, given that the platform supports it.
        return BigInt(Math.round(1000 * (now + correction))) * 1000n;
      };
    })(),
"""


BOOK_MAIN_DECL_ANCHOR = "  /// Callbacks from Web Workers (each one representing one task).\n"
BOOK_MAIN_DECL = """  /// What the Workers asked of the main thread, lately, in the order it was asked (tools/kernel-flags.js reads it
  /// out of a machine that has stopped): [the time in ms, what, the task switched from or released, the task
  /// switched to or made]. What: S a switch, C a switch to a new task, R a release, K a Worker ended, U a CPU
  /// started, X a CPU stopped. And the name of each task's runner, by the task.
  const switch_log = [];
  const runner_names = {};
  const switch_note = (what, prev, next) => {
    if (switch_log.length >= 4000) {
      switch_log.splice(0, 1000);
    }
    switch_log.push([performance.now(), what, prev, next]);
  };
  const runner_name_of = (task) => (runner_names[task] || "a task without a runner") + " [" + task + "]";
  // ?runners=old on the page: the two things put right below are left as they were (to compare against).
  const runners_fixed = !(typeof location != "undefined" && /[?&]runners=old(&|$)/.test(location.search));

"""
BOOK_MAIN_START_OLD = """      log("Starting cpu " + message.cpu + " (" + message.idle_task + ")");
      make_cpu(message.cpu, message.idle_task);
"""
BOOK_MAIN_START_NEW = """      log("Starting cpu " + message.cpu + " (" + message.idle_task + ")");
      switch_note("U", message.cpu, message.idle_task);
      make_cpu(message.cpu, message.idle_task);
"""
BOOK_MAIN_STOP_OLD = """      if (cpus[message.cpu]) {
        log("[Main]: Stopping CPU " + message.cpu);
"""
BOOK_MAIN_STOP_NEW = """      switch_note("X", message.cpu, 0);
      if (cpus[message.cpu]) {
        log("[Main]: Stopping CPU " + message.cpu);
"""
BOOK_MAIN_CREATE_OLD = """    create_and_run_task: (message) => {
      // ret_from_fork will make sure the task switch finishes.
      make_task(message.prev_task, message.new_task, message.name, message.user_executable);
    },

    release_task: (message) => {
      kill_task(message.dead_task);
    },

    serialize_tasks: (message) => {
      // next_task was previously suspended, wake it up.

      // Tell the next task where we switched from, so that it can finish the task switch.
      tasks[message.next_task].last_task[0] = message.prev_task;
"""
BOOK_MAIN_CREATE_NEW = """    create_and_run_task: (message) => {
      switch_note("C", message.prev_task, message.new_task);
      if (!tasks[message.prev_task]) {
        log("[Main]: create_and_run_task " + message.prev_task + " -> " + message.new_task + ": the task switched from has no runner");
      } else if (!tasks[message.prev_task].running) {
        log("[Main]: create_and_run_task " + message.prev_task + " -> " + message.new_task + ": " + runner_name_of(message.prev_task) + " is held not to be running");
      }

      // ret_from_fork will make sure the task switch finishes.
      make_task(message.prev_task, message.new_task, message.name, message.user_executable);

      // The task that made the switch stops, as in serialize_tasks below. (It was not accounted for here: a dying
      // task whose last switch was to a new task kept its Worker, blocked in serialize_me(), for good.)
      if (runners_fixed && tasks[message.prev_task]) {
        tasks[message.prev_task].running = false;
        if (tasks[message.prev_task].kill) {
          kill_task(message.prev_task);
        }
      }
    },

    release_task: (message) => {
      switch_note("R", message.dead_task, 0);
      if (!tasks[message.dead_task]) {
        log("[Main]: release_task " + message.dead_task + ": no runner");
        return;
      }
      kill_task(message.dead_task);
    },

    serialize_tasks: (message) => {
      // next_task was previously suspended, wake it up.
      switch_note("S", message.prev_task, message.next_task);
      if (!tasks[message.next_task] || !tasks[message.prev_task]) {
        log("[Main]: serialize_tasks " + message.prev_task + " -> " + message.next_task + ": "
            + (tasks[message.prev_task] ? "" : "the task switched from has no runner; ")
            + (tasks[message.next_task] ? "" : "the task switched to has no runner"));
        return;
      }
      if (tasks[message.next_task].running) {
        log("[Main]: serialize_tasks " + message.prev_task + " -> " + message.next_task + ": " + runner_name_of(message.next_task) + " is held to be running already");
      }
      if (!tasks[message.prev_task].running) {
        log("[Main]: serialize_tasks " + message.prev_task + " -> " + message.next_task + ": " + runner_name_of(message.prev_task) + " is held not to be running");
      }

      // Tell the next task where we switched from, so that it can finish the task switch.
      tasks[message.next_task].last_task[0] = message.prev_task;
"""
BOOK_MAIN_CPU_OLD = """    const runner = make_vmlinux_runner(name, options);
    cpus[cpu] = runner;
"""
BOOK_MAIN_CPU_NEW = """    const runner = make_vmlinux_runner(name, options);
    cpus[cpu] = runner;
    if (idle_task) {
      runner_names[idle_task] = name;
    }
"""
BOOK_MAIN_TASK_OLD = """    // Use a spare runner if there is one: it has started and instantiated vmlinux already.
    const spare = spare_runners.shift();
"""
BOOK_MAIN_TASK_NEW = """    // A task_struct's address comes round again when the kernel reuses the memory of a task that is gone. A runner
    // still registered for it is a Worker the kernel has forgotten about: say so, and end it.
    if (tasks[new_task]) {
      const old = tasks[new_task];
      log("[Main]: task " + new_task + " (" + runner_name + ") is made while " + runner_name_of(new_task)
          + " is still registered (running " + old.running + ", kill " + old.kill + ")" + (runners_fixed ? ": that Worker is terminated" : ""));
      if (runners_fixed) {
        old.worker.terminate();
        delete tasks[new_task];
      }
    }
    runner_names[new_task] = runner_name;

    // Use a spare runner if there is one: it has started and instantiated vmlinux already.
    const spare = spare_runners.shift();
"""
BOOK_MAIN_KILL_OLD = """  const kill_task = (dead_task) => {
    const task = tasks[dead_task];
    if (task.running) {
"""
BOOK_MAIN_KILL_NEW = """  const kill_task = (dead_task) => {
    const task = tasks[dead_task];
    switch_note("K", dead_task, task.running ? 1 : 0);
    if (task.running) {
"""
BOOK_MAIN_RETURN_OLD = """    memory: memory,
    disks: disks,
"""
BOOK_MAIN_RETURN_NEW = """    memory: memory,
    disks: disks,
    switches: switch_log,
    runner_names: runner_names,
    runners: tasks,
"""


SIGNALS_WORKER_OLD = """    wasm_user_mode_tail: (flow) => {
      if (flow == -1) {
        // Exec has been called and we should not return from the syscall. Trap() to collapse the call stack of the user
        // executable. When swallowed, run the new user executable that was already preloaded by wasm_load_executable().
        // This takes precedence of signal handlers or signal return - no reason to run any old user code!
        throw new Trap("reload_program");
      } else if (flow >= 1 && flow <= 3) {
        // First, handle any signal (possibly stacked). Then, handle any signal return (happens after stacked signals).
        // If exec() happens, we will slip out in the catch-else clause, ensuring the sigreturn does not proceed.
        if (flow & 1) {
          try {
            if (user_executable_instance.exports.__libc_handle_signal) {
              // Setup signal frame...
              user_executable_imports.env.__stack_pointer.value = vmlinux_instance.exports.get_user_stack_pointer();
              user_executable_instance.exports.__set_tls_base(vmlinux_instance.exports.get_user_tls_base());

              user_executable_instance.exports.__libc_handle_signal();
              throw new Error("Wasm function __libc_handle_signal() returned (it should never return)!");
            } else {
              throw new Error("Wasm function __libc_handle_signal() not defined!");
            }
          } catch (error) {
            if (error instanceof Trap && error.kind == "signal_return") {
              // ...restore signal frame.
              user_executable_imports.env.__stack_pointer.value = vmlinux_instance.exports.get_user_stack_pointer();
              user_executable_instance.exports.__set_tls_base(vmlinux_instance.exports.get_user_tls_base());
            } else {
              // Either a genuine error, or a Trap() from exec() (signal handlers are allowed to call exec()).
              throw error;
            }
          }
        }

        if (flow & 2) {
          throw new Trap("signal_return");
        }
      } else {
"""
SIGNALS_WORKER_NEW = """    wasm_user_mode_tail: (flow) => {
      if (flow == -1) {
        // Exec has been called and we should not return from the syscall. Trap() to collapse the call stack of the user
        // executable. When swallowed, run the new user executable that was already preloaded by wasm_load_executable().
        // This takes precedence of signal handlers or signal return - no reason to run any old user code!
        throw new Trap("reload_program");
      } else if (flow >= 1 && flow <= 3) {
        // A signal return (a handler's sigreturn, 2): the handler is done, and its frames on the native stack, which
        // nothing can restore the way sigreturn restores the program's stack, are unwound by a Trap caught by the tail
        // that started the handler. When the kernel has another signal to deliver at the same time (3), that tail
        // delivers it once the frames are gone (below), where the old handler was: so handlers that keep coming, a
        // sigreturn apart, run one after the other, not one inside the other. (They did run nested, until the native
        // stack ran out: rv's stop-the-world kick comes every 2 ms to a thread that has not parked yet, and a thread
        // on a starved CPU takes longer than that to return from anything.) ?signals=old: the old order.
        if ((flow & 2) && !signals_old) {
          const trap = new Trap("signal_return");
          trap.deliver = (flow & 1) != 0;
          throw trap;
        }
        if (flow & 1) {
          let deliver = true;
          while (deliver) {
            deliver = false;
            try {
              if (user_executable_instance.exports.__libc_handle_signal) {
                // Setup signal frame...
                user_executable_imports.env.__stack_pointer.value = vmlinux_instance.exports.get_user_stack_pointer();
                user_executable_instance.exports.__set_tls_base(vmlinux_instance.exports.get_user_tls_base());

                user_executable_instance.exports.__libc_handle_signal();
                throw new Error("Wasm function __libc_handle_signal() returned (it should never return)!");
              } else {
                throw new Error("Wasm function __libc_handle_signal() not defined!");
              }
            } catch (error) {
              if (error instanceof Trap && error.kind == "signal_return") {
                // ...restore signal frame (or take up the next signal's, which the kernel has set up on the stack
                // the sigreturn restored).
                user_executable_imports.env.__stack_pointer.value = vmlinux_instance.exports.get_user_stack_pointer();
                user_executable_instance.exports.__set_tls_base(vmlinux_instance.exports.get_user_tls_base());
                deliver = error.deliver === true;
              } else {
                // Either a genuine error, or a Trap() from exec() (signal handlers are allowed to call exec()).
                throw error;
              }
            }
          }
        }

        if (flow & 2) {
          throw new Trap("signal_return");
        }
      } else {
"""
SIGNALS_WORKER_DECL_ANCHOR = "  let runner_name = \"[Unknown]\";\n"
SIGNALS_WORKER_DECL = """  let runner_name = "[Unknown]";

  /// ?signals=old on the page (see wasm_user_mode_tail): a new signal's handler runs inside the old one's frames.
  let signals_old = false;
"""
SIGNALS_WORKER_INIT_ANCHOR = "      runner_name = message.runner_name;\n      memory = message.memory;\n"
SIGNALS_WORKER_INIT = """      runner_name = message.runner_name;
      signals_old = message.signals_old === true;
      memory = message.memory;
"""
SIGNALS_MAIN_OLD = """      last_task: last_task,
      runner_name: name,
    });
"""
SIGNALS_MAIN_NEW = """      last_task: last_task,
      runner_name: name,
      signals_old: typeof location != "undefined" && /[?&]signals=old(&|$)/.test(location.search),
    });
"""

CRASH_KERNEL_OLD = """        if (vmlinux_instance) {
          vmlinux_instance.exports.raise_exception();
          throw new Error("raise_exception() returned");
        } else {
"""
CRASH_KERNEL_NEW = """        if (vmlinux_instance) {
          // The program's fault, or the kernel's? The CPU's flags say: in user mode, the program threw, and the
          // kernel handles that as an exception of the program. Otherwise the error came out of the kernel itself,
          // in the middle of a system call or a task switch: whatever it holds is still held, and the task the
          // scheduler may already have woken for this CPU runs in its own Worker. Running the kernel again from here
          // would make two of them as one CPU (which is what "Syscall called when in kernel mode" was). Say so, and
          // let this Worker be: the machine will show the task as hung, which is what it is.
          const flags = vmlinux_instance.exports.wasm_cpuflags_now ? vmlinux_instance.exports.wasm_cpuflags_now() : 2;
          if (!(flags & 2)) {
            log("crashed INSIDE THE KERNEL (the cpu's flags are 0x" + flags.toString(16) + "): this Worker stops here; its task is lost");
            port.postMessage({ method: "kernel_crashed", runner_name: runner_name, error: error.toString() });
            return;
          }
          vmlinux_instance.exports.raise_exception();
          throw new Error("raise_exception() returned");
        } else {
"""
CRASH_KERNEL_MAIN_ANCHOR = "    console_write: (message) => {\n"
CRASH_KERNEL_MAIN = """    kernel_crashed: (message) => {
      // A Worker threw inside the kernel (linux-worker.js, wasm_error): kept for the tools that read the machine.
      switch_note("E", 0, 0);
      kernel_crashes.push(message.runner_name + ": " + message.error);
    },

"""
CRASH_KERNEL_MAIN_DECL_ANCHOR = "  const runner_name_of = (task) =>"
CRASH_KERNEL_MAIN_DECL = """  /// Workers that threw inside the kernel (their names and errors), for tools/kernel-flags.js.
  const kernel_crashes = [];
"""
CRASH_KERNEL_MAIN_RETURN_OLD = """    switches: switch_log,
    runner_names: runner_names,
"""
CRASH_KERNEL_MAIN_RETURN_NEW = """    switches: switch_log,
    runner_names: runner_names,
    kernel_crashes: kernel_crashes,
"""

JS_WORKER_DECL_ANCHOR = "  /// Flag that a clone callback should be called instead of _start().\n"
JS_WORKER_DECL = """  /// A JavaScript program handed over by __run_js (see there), or null.
  let js_program = null;

  /// What the kernel's callbacks need of the browser, as it was when this Worker started: a JavaScript program
  /// shares the global object with this script, and such programs do replace what is there (clocks most of all).
  const host_performance_now = performance.now.bind(performance);
  const host_time_origin = performance.timeOrigin;
  const host_date_now = Date.now.bind(Date);
  const host_random_values = crypto.getRandomValues.bind(crypto);

"""
JS_WORKER_PORT_OLD = "  let port = self;\n"
JS_WORKER_PORT_NEW = """  // (Not the global object itself: a JavaScript program may take from it what only a browser's worker has.)
  let port = { postMessage: self.postMessage.bind(self) };
"""
JS_WORKER_CLOCK_OLD = "        const now = performance.timeOrigin + performance.now();\n        const apart = Date.now() - (now + correction);\n"
JS_WORKER_CLOCK_NEW = "        const now = host_time_origin + host_performance_now();\n        const apart = host_date_now() - (now + correction);\n"
JS_WORKER_RANDOM_OLD = "      crypto.getRandomValues(data);\n"
JS_WORKER_RANDOM_NEW = "      host_random_values(data);\n"
JS_WORKER_IMPORT = """            // JavaScript as a program (hostnode/): stop running this program, as exec does, and run the JavaScript
            // text at [ptr, ptr + len) as what this task does from now on (see js_program_run). arg is for the
            // text: where the program has put what it wants to tell it. Does not return.
            __run_js: (ptr, len, arg) => {
              const start = Number(ptr);
              js_program = {
                source: text_decoder.decode(new Uint8Array(memory.buffer).slice(start, start + Number(len))),
                arg: arg,
              };
              throw new Trap("run_js");
            },

"""
JS_WORKER_RUN_ANCHOR = "      const user_executable_error = (error) => {\n"
JS_WORKER_RUN = """      // Run the JavaScript a program handed over with __run_js. The text is an expression for a function, which
      // is called once with what a process needs. It is called from here, a reaction to a promise, with nothing
      // of Wasm on the stack: when it returns, the jobs of the promises it made run, and what it has asked the
      // browser to call later is called later, so it can be a program with an event loop of its own. The Wasm
      // program stays instantiated: the kernel delivers signals to it (wasm_user_mode_tail), and its C library
      // is there to be called.
      const js_program_run = () => {
        const program = js_program;
        js_program = null;
        try {
          const main = (0, eval)(program.source);
          main({
            memory: memory,
            instance: user_executable_instance,
            stack_pointer: user_executable_imports.env.__stack_pointer,
            // (stack pointer, thread pointer, number, arguments...) => result, by the number of arguments.
            syscalls: [0, 1, 2, 3, 4, 5, 6].map((n) => vmlinux_instance.exports["wasm_syscall_" + n]),
            arg: program.arg,
            name: runner_name,
            log: log,
            // For what the text catches and cannot handle: a Trap of this script (exec, panic), or an error
            // that is the end of the program.
            error: user_executable_error,
          });
        } catch (error) {
          return user_executable_error(error);
        }
      };

"""
JS_WORKER_TRAP_ANCHOR = """          } else if (error.kind == "panic") {\n"""
JS_WORKER_TRAP = """          } else if (error.kind == "run_js") {
            return js_program_run();
"""


SWITCH_WORKER_DECL_ANCHOR = "  /// SAB-backed storage for last process in switch_to (when it returns back from another task).\n"
SWITCH_WORKER_DECL = """  /// Task switches without the main thread (see wasm_serialize_tasks), unless the page says ?switches=main.
  let switches_main = false;
  /// Where the main thread finds the kernel's IRQ_CPU interrupt word (see wasm_driver_input_rx), or null.
  let doorbell = null;
  /// The ring of switches all Workers share, for the book (linux.js): index[0] counts notes, each note is
  /// [the time (ms since 1970, fractional), the task switched from, the task switched to].
  let switch_ring = null;
  /// A dead task's Worker waits on this for good: nobody wakes it, the main thread ends the Worker.
  const switch_never = new Int32Array(new SharedArrayBuffer(4));
  /// The clock for the notes, as it was when this Worker started (a JavaScript program may replace it).
  const switch_now = performance.now.bind(performance);
  const switch_origin = performance.timeOrigin;

"""

SWITCH_WORKER_HELPERS_ANCHOR = "  /// Callbacks from within Linux/Wasm out to our host code (cpu is not neccessarily ours).\n"
SWITCH_WORKER_HELPERS = """  /// The words of a task's thread_info a switch to it goes through (the kernel's wasm_task_switch_area():
  /// switch_from, 8 bytes, then switch_word), 0 for a dead task, or -1: the old way (through the main thread).
  const switch_area = (task) => {
    const exports = vmlinux_instance && vmlinux_instance.exports;
    if (switches_main || arch_bits != 64 || !exports || !exports.wasm_task_switch_area) {
      return -1;
    }
    return Number(exports.wasm_task_switch_area(Ulong(task)));
  };

  const switch_note = (prev_task, next_task) => {
    if (switch_ring) {
      const n = Atomics.add(switch_ring.index, 0, 1);
      const at = (n % switch_ring.slots) * 3;
      switch_ring.notes[at] = switch_origin + switch_now();
      switch_ring.notes[at + 1] = prev_task;
      switch_ring.notes[at + 2] = next_task;
    }
  };

  /// Wake the task whose switch words are at `area`, telling it it was switched to from prev_task.
  const switch_wake = (area, prev_task) => {
    const buffer = memory.buffer;
    new BigUint64Array(buffer, area, 1)[0] = BigInt(prev_task);
    const word = new Int32Array(buffer, area + 8, 1);
    Atomics.store(word, 0, 1);
    Atomics.notify(word, 0, 1);
  };

  /// A dead task's last switch: the main thread hears it once, AFTER the next task has been woken (or made): the
  /// main thread may end this Worker as soon as it hears it. (A late task_done, after a new task was made at the
  /// same address, is told apart by the Worker it comes from: see linux.js.)
  let switch_done_said = false;
  const switch_done = (task) => {
    if (!switch_done_said) {
      switch_done_said = true;
      port.postMessage({ method: "task_done", task: task });
    }
  };

  /// Wait until some task switches to ours (whose words are at `area`), and return the task it came from. A
  /// task that is dead (area 0) is never switched to again: say so to the main thread, and wait for the end.
  const switch_wait = (area, prev_task) => {
    if (!area) {
      switch_done(prev_task);
      for (;;) {
        Atomics.wait(switch_never, 0, 0);
      }
    }
    for (;;) {
      const word = new Int32Array(memory.buffer, area + 8, 1);
      while (Atomics.exchange(word, 0, 0) == 0) {
        Atomics.wait(word, 0, 0);
      }
      const from = Number(new BigUint64Array(memory.buffer, area, 1)[0]);
      if (switch_woken_right(prev_task, from)) {
        return from;
      }
    }
  };

  /// After a Worker was woken to run its task: is the task its CPU's current task now (the kernel's
  /// wasm_task_switched_in)? A wake for a switch that did not happen (one that came twice) would make two Workers
  /// run as one CPU, which the kernel panics about ("Not the current task (switched to)"). Such a wake is said in
  /// the log, and the Worker waits again for the real one. True if all is well (or the kernel cannot tell).
  let switch_wrong_wakes = 0;
  const switch_woken_right = (task, from) => {
    const exports = vmlinux_instance && vmlinux_instance.exports;
    if (!exports || !exports.wasm_task_switched_in || exports.wasm_task_switched_in(Ulong(task))) {
      return true;
    }
    switch_wrong_wakes++;
    const curr = exports.wasm_task_cpu_curr ? Number(exports.wasm_task_cpu_curr(Ulong(task))) : 0;
    log("woken for a switch that did not happen (" + switch_wrong_wakes + "): task " + task + " (from " + from
        + "), its cpu's current task is " + curr + "; waiting again");
    return false;
  };

  /// Tell the main thread where the kernel takes the interrupts the host raises (see linux.js: raise_irq).
  const doorbell_advertise = () => {
    if (doorbell && !Atomics.load(doorbell, 0) && vmlinux_instance && vmlinux_instance.exports.wasm_irq_cpu_word) {
      Atomics.store(doorbell, 0, BigInt(vmlinux_instance.exports.wasm_irq_cpu_word()));
    }
  };

"""

SWITCH_WORKER_CREATE_OLD = """      // Serialize this (old) task.
      return serialize_me();
    },

    /// Remove a task created by wasm_create_and_run_task().
"""
SWITCH_WORKER_CREATE_NEW = """      // Serialize this (old) task.
      return direct ? switch_wait(prev_area, prev_task) : serialize_me(prev_task);
    },

    /// Remove a task created by wasm_create_and_run_task().
"""
SWITCH_WORKER_CREATE_POST_OLD = """      // Tell main to create the new task, and then run it for the first time!
      port.postMessage({
        method: "create_and_run_task",
"""
SWITCH_WORKER_CREATE_POST_NEW = """      // Tell main to create the new task, and then run it for the first time!
      const prev_area = switch_area(prev_task);
      const direct = prev_area >= 0;
      if (direct) {
        switch_note(prev_task, new_task);
      }
      port.postMessage({
        method: "create_and_run_task",
        direct: direct,
"""
SWITCH_WORKER_SERIALIZE_OLD = """    wasm_serialize_tasks: (prev_task, next_task) => {
      // Notify the next task that it can run again.
"""
SWITCH_WORKER_SERIALIZE_NEW = """    wasm_serialize_tasks: (prev_task, next_task) => {
      // Without the main thread: wake the next task's Worker ourselves, then wait for our turn.
      const next_area = switch_area(next_task);
      if (next_area > 0) {
        const prev_area = switch_area(prev_task);
        switch_note(prev_task, next_task);
        switch_wake(next_area, prev_task);
        return switch_wait(prev_area, prev_task);
      }

      // Notify the next task that it can run again.
"""
SWITCH_WORKER_SERIALIZE_TAIL_OLD = """      // Serialize this (old) task.
      return serialize_me();
    },

    /// Kernel panic. We can't proceed.
"""
SWITCH_WORKER_SERIALIZE_TAIL_NEW = """      // Serialize this (old) task.
      return serialize_me(prev_task);
    },

    /// Kernel panic. We can't proceed.
"""
SWITCH_WORKER_SERIALIZE_ME_OLD = """  const serialize_me = () => {
    // Wait for some other task or CPU to wake us up.
    lock_wait("serialize");
    return switch_to_last_task[0];  // last_task was written by the caller just prior to waking.
  };
"""
SWITCH_WORKER_SERIALIZE_ME_NEW = """  const serialize_me = (task) => {
    // Wait for some other task or CPU to wake us up (again, if it was not a real switch: switch_woken_right).
    for (;;) {
      lock_wait("serialize");
      const from = switch_to_last_task[0];  // last_task was written by the caller just prior to waking.
      if (task === undefined || switch_woken_right(task, from)) {
        return from;
      }
    }
  };
"""
SWITCH_WORKER_INIT_ANCHOR = "      signals_old = message.signals_old === true;\n"
SWITCH_WORKER_INIT = """      signals_old = message.signals_old === true;
      switches_main = message.switches_main === true;
      switch_ring = message.switch_ring || null;
      doorbell = message.doorbell || null;
"""
DOORBELL_WORKER_INPUT_OLD = """    wasm_driver_input_rx: (events, max) => {
      if (!input) {
        return 0;
      }
"""
DOORBELL_WORKER_INPUT_NEW = """    wasm_driver_input_rx: (events, max) => {
      if (!input) {
        return 0;
      }
      doorbell_advertise();
"""
DOORBELL_WORKER_NET_OLD = """    wasm_driver_net_rx: (buffer, size) => {
      if (!net) {
        return 0;
      }
"""
DOORBELL_WORKER_NET_NEW = """    wasm_driver_net_rx: (buffer, size) => {
      if (!net) {
        return 0;
      }
      doorbell_advertise();
"""

SWITCH_MAIN_DECL_ANCHOR = "  const runner_name_of = (task) =>"
SWITCH_MAIN_DECL = """  /// The ring the Workers write their switches into (linux-worker.js: switch_note), and the book with it: the
  /// main thread's own notes and the ring's, in the order of their times (each [ms, what, from, to]).
  const switch_ring = { slots: 4096, index: new Int32Array(new SharedArrayBuffer(4)) };
  switch_ring.notes = new Float64Array(new SharedArrayBuffer(switch_ring.slots * 3 * 8));
  const switches_all = () => {
    const notes = switch_log.slice();
    const n = Atomics.load(switch_ring.index, 0);
    const origin = performance.timeOrigin;
    for (let i = Math.max(0, n - switch_ring.slots); i < n; i++) {
      const at = (i % switch_ring.slots) * 3;
      notes.push([switch_ring.notes[at] - origin, "S", switch_ring.notes[at + 1], switch_ring.notes[at + 2]]);
    }
    return notes.sort((a, b) => a[0] - b[0]);
  };
  // ?switches=main on the page (globalThis.rv_query where there is no page): every switch through the main thread.
  const page_query = typeof location != "undefined" ? location.search : (globalThis.rv_query || "");
  const switches_main = /[?&]switches=main(&|$)/.test(page_query);

  /// Interrupts the host raises (WASM_IRQ_INPUT 2, WASM_IRQ_NET 3: something was put into a ring): the bit in
  /// the word of the kernel's IRQ_CPU, and a notify, as the kernel's raise_interrupt() does. A Worker has put
  /// the word's address into doorbell[0] (0: not yet, or a kernel that takes no such interrupts).
  const doorbell = new BigUint64Array(new SharedArrayBuffer(8));
  const IRQ_INPUT = 2, IRQ_NET = 3;
  const raise_irq = (irq) => {
    const at = Number(Atomics.load(doorbell, 0));
    if (at) {
      const buffer = memory.buffer;
      Atomics.or(new BigInt64Array(buffer, at, 1), 0, 1n << BigInt(irq));
      Atomics.notify(new Int32Array(buffer, at, 1), 0, 1);
    }
  };

"""
SWITCH_MAIN_CREATE_OLD = "      if (runners_fixed && tasks[message.prev_task]) {\n"
SWITCH_MAIN_CREATE_NEW = """      // (Not when the Workers switch by themselves: the task that made the switch may well run again, and
      // its Worker says when it is done, task_done.)
      if (runners_fixed && !message.direct && tasks[message.prev_task]) {
"""
SWITCH_MAIN_DONE_ANCHOR = "    release_task: (message) => {\n"
SWITCH_MAIN_DONE = """    task_done: (message, worker) => {
      // A dead task's Worker has made its last switch (linux-worker.js: switch_wait): it can be ended once the
      // task is released, which may have happened already. (Only if that Worker is still the one registered for
      // the address: a new task made at the same address has a runner of its own, and the old one was ended.)
      switch_note("D", message.task, 0);
      const task = tasks[message.task];
      if (task && task.worker === worker) {
        task.running = false;
        if (task.kill) {
          kill_task(message.task);
        }
      }
    },

"""
SWITCH_MAIN_INIT_ANCHOR = "      signals_old: typeof location"
SWITCH_MAIN_INIT = """      switches_main: switches_main,
      switch_ring: switch_ring,
      doorbell: doorbell,
"""
SWITCH_MAIN_RETURN_OLD = "    switches: switch_log,\n"
SWITCH_MAIN_RETURN_NEW = "    get switches() { return switches_all(); },\n"
DOORBELL_MAIN_INPUT_OLD = "    Atomics.store(input.index, 0, (written + 1) | 0);\n  };\n"
DOORBELL_MAIN_INPUT_NEW = "    Atomics.store(input.index, 0, (written + 1) | 0);\n    raise_irq(IRQ_INPUT);\n  };\n"
DOORBELL_MAIN_NET_OLD = "      Atomics.store(net.index, 0, (written + 1) | 0);\n    };\n"
DOORBELL_MAIN_NET_NEW = "      Atomics.store(net.index, 0, (written + 1) | 0);\n      raise_irq(IRQ_NET);\n    };\n"


STALE_WORKER_LOCK_OLD = """  const lock_wait = (lock) => {
    Atomics.wait(locks._memory, locks[lock], 0);
    Atomics.store(locks._memory, locks[lock], 0);
  };
"""
STALE_WORKER_LOCK_NEW = """  const lock_wait = (lock) => {
    // Take the wake (1 -> 0); a wait that ends with nothing to take was a late notify of an older wake (14).
    while (Atomics.exchange(locks._memory, locks[lock], 0) == 0) {
      Atomics.wait(locks._memory, locks[lock], 0);
    }
  };
"""
STALE_MAIN_LOCK_OLD = """  const lock_wait = (locks, lock) => {
    Atomics.wait(locks._memory, locks[lock], 0);
    Atomics.store(locks._memory, locks[lock], 0);
  };
"""
STALE_MAIN_LOCK_NEW = """  const lock_wait = (locks, lock) => {
    while (Atomics.exchange(locks._memory, locks[lock], 0) == 0) {
      Atomics.wait(locks._memory, locks[lock], 0);
    }
  };
"""
STALE_WORKER_HVC_OLD = """      Atomics.wait(console_read_messenger, 0, -1);
"""
STALE_WORKER_HVC_NEW = """      while (Atomics.load(console_read_messenger, 0) == -1) {
        Atomics.wait(console_read_messenger, 0, -1);
      }
"""

MEMORY_MAX_OLD = "    maximum: Ulong(0x10000), // Allow the full 32-bit address space to be allocated.\n"
MEMORY_MAX_NEW = """    // As much as V8 lets a 64-bit memory have, 16 GiB (kV8MaxWasmMemory64Pages), which is also the maximum every
    // program is linked with; the full 32-bit address space for a 32-bit one. The kernel grows it to its size.
    maximum: Ulong(arch_bits == 64 ? 0x40000 : 0x10000),
"""


SIGLOOK_DECL_ANCHOR = "  /// The ring of input events"
SIGLOOK_DECL = """  /// For telling what was wrong when a signal's handler cannot be called (18): how many programs this Worker has
  /// loaded, the size of the one it runs, and whether it began as a spare runner.
  let programs_loaded = 0;
  let program_size = 0;
  let was_spare = false;
  let null_page_said = false;
  /// Whether the program running here has musl's old __libc_handle_signal (18; see wasm_load_executable).
  let old_signal_read = true;
  /// ?sigfix=0 on the page: 18 and 19 do nothing but look (no signal delivered by the runtime, no slot refused, no
  /// thread stack moved), for telling whether they are what a fault is about.
  let sigfix_off = false;
  /// The slots of the program's own functions: the table as it was when the program was instantiated (18).
  let program_slots = 4096;

"""
SIGLOOK_LOAD_OLD = """    wasm_load_executable: (bin_start, bin_end, data_start, table_start) => {
"""
SIGLOOK_LOAD_NEW = """    wasm_load_executable: (bin_start, bin_end, data_start, table_start) => {
      programs_loaded++;
      program_size = Number(bin_end) - Number(bin_start);
      // (18) Was the program linked with the musl whose __libc_handle_signal reads info_param from address 12 (the
      // code of that read: global.get 0, i64.const 4, i64.const 8, i64.add, i64.load)? Only then does the runtime
      // deliver its signals itself; a program linked with the fixed musl is left to its own.
      {
        const code = new Uint8Array(memory.buffer, Number(bin_start), program_size);
        const look = [0x23, 0x80, 0x80, 0x80, 0x80, 0x00, 0x42, 0x04, 0x42, 0x08, 0x7c, 0x29, 0x03, 0x00];
        old_signal_read = false;
        for (let at = code.indexOf(look[0]); at >= 0 && at + look.length <= code.length; at = code.indexOf(look[0], at + 1)) {
          let k = 1;
          while (k < look.length && code[at + k] == look[k]) {
            k++;
          }
          if (k == look.length) {
            old_signal_read = true;
            break;
          }
        }
      }
"""
SIGLOOK_INIT_OLD = """      signals_old = message.signals_old === true;
"""
SIGLOOK_INIT_NEW = """      signals_old = message.signals_old === true;
      sigfix_off = message.sigfix_off === true;
"""
SIGLOOK_MAIN_OLD = """      signals_old: typeof location"""
SIGLOOK_MAIN_NEW = """      sigfix_off: typeof location != "undefined" && /[?&]sigfix=0(&|$)/.test(location.search),
      signals_old: typeof location"""
SIGLOOK_SPARE_OLD = """        message_callbacks.assign = (assignment) => {
"""
SIGLOOK_SPARE_NEW = """        was_spare = true;
        message_callbacks.assign = (assignment) => {
"""
SIGLOOK_CALL_OLD = """                user_executable_instance.exports.__libc_handle_signal();
                throw new Error("Wasm function __libc_handle_signal() returned (it should never return)!");
"""
SIGLOOK_CALL_NEW = """                // (18) The frame, read here: musl's __libc_handle_signal reads its info_param from address 12
                // of memory (8 on wasm32) instead of the frame (a stray constant in src/signal/wasm/restore.S), so
                // it calls the handler right only while that word is 0 and the handler is one without SA_SIGINFO.
                // Otherwise the handler is called from here, as musl means to, and then rt_sigreturn made, as
                // musl's __restore_rt does.
                let frame_said = null;
                let musl_reads = 0n;
                try {
                  const sp = Number(user_executable_imports.env.__stack_pointer.value);
                  const at = arch_bits == 64 ? 8 : 4;
                  const view = new DataView(memory.buffer);
                  const word = (offset) => arch_bits == 64 ? view.getBigUint64(offset, true) : BigInt(view.getUint32(offset, true));
                  frame_said = { sp: sp, sig: view.getInt32(sp, true), info: word(sp + at), uc: word(sp + 2 * at),
                                 handler: Number(word(sp + 3 * at)) };
                  frame_said.siginfo = frame_said.info != 0n;
                  musl_reads = word(arch_bits == 64 ? 12 : 8);
                  if (old_signal_read && musl_reads != 0n && !null_page_said) {
                    null_page_said = true;
                    const words = [];
                    for (let k = 0; k < 8; k++) {
                      words.push(word(k * at).toString(16));
                    }
                    log("null page error: something wrote into the first bytes of memory (" + words.join(" ") +
                        "); signals are delivered by the runtime instead of musl from now on (18)");
                  }
                } catch (error) {
                  frame_said = { error: String(error) };
                }
                try {
                  if (frame_said.error || sigfix_off || !old_signal_read || (!frame_said.siginfo && musl_reads == 0n)) {
                    user_executable_instance.exports.__libc_handle_signal();
                  } else {
                    const handler = user_executable_imports.env.__indirect_function_table.get(frame_said.handler);
                    if (frame_said.siginfo) {
                      handler(frame_said.sig, Ulong(frame_said.info), Ulong(frame_said.uc));
                    } else {
                      handler(frame_said.sig);
                    }
                    // __restore_rt: rt_sigreturn (139) with the program's stack pointer and TLS base.
                    const tls = user_executable_instance.exports.__get_tls_base ?
                      user_executable_instance.exports.__get_tls_base() : vmlinux_instance.exports.get_user_tls_base();
                    vmlinux_instance.exports.wasm_syscall_0(user_executable_imports.env.__stack_pointer.value, tls, Ulong(139));
                  }
                } catch (error) {
                  if (!(error instanceof Trap) && /null function|signature mismatch/.test(String(error))) {
                    signal_trouble(frame_said, error);
                  }
                  throw error;
                }
                throw new Error("Wasm function __libc_handle_signal() returned (it should never return)!");
"""
SIGLOOK_HELPER_ANCHOR = """  /// Get a JS string object from a (nul-terminated) C-string in a Uint8Array.
"""
SIGLOOK_HELPER = """  /// A signal's handler could not be called (18): say what the frame named, what the table has there, and which
  /// program this Worker runs. (Called after the trap, from the tail that delivered the signal.)
  const signal_trouble = (said, error) => {
    let what = "";
    if (said && !said.error) {
      const table = user_executable_imports && user_executable_imports.env.__indirect_function_table;
      let has;
      try {
        const entry = table.get(said.handler);
        has = entry ? "function " + entry.name + " with " + entry.length + " parameters" : "nothing";
      } catch (e) {
        has = "no such slot";
      }
      what = "signal " + said.sig + " (" + (said.siginfo ? "SA_SIGINFO" : "no SA_SIGINFO") + "), handler slot " +
        said.handler + (said.handler >= 4096 ? " (where translated code goes)" : "") + ", which has " + has +
        "; the table has " + (table ? table.length : "?") + " slots; the frame is at " + said.sp;
      // (The frame as it is now: if it is not what it was a moment before the call, something else wrote it.)
      try {
        const frame = new DataView(memory.buffer, said.sp, 32);
        what += "; the frame now says signal " + frame.getInt32(0, true) + ", info " + frame.getBigUint64(8, true) +
          ", handler " + frame.getBigUint64(24, true);
      } catch (e) {
        what += "; the frame cannot be read now";
      }
    } else {
      what = "the frame could not be read (" + (said && said.error) + ")";
    }
    log("signal handler crash (" + error.message + "): " + what + "; the program is the " +
        programs_loaded + "th this Worker loaded, " + program_size + " bytes, " +
        (typeof jit_cache_ok != "undefined" && jit_cache_ok ? "the emulator" : "not known to be the emulator") +
        "; the Worker " + (was_spare ? "began" : "did not begin") + " as a spare runner");
  };

"""
SIGLOOK_RELEASE_OLD = """                  user_executable_imports.env.__indirect_function_table.set(Number(slot), null);
"""
SIGLOOK_RELEASE_NEW = """                  if (Number(slot) < program_slots && !sigfix_off) {
                    log("__rv_jit_install error: asked to empty slot " + slot + ", one of the program's own: refused (18)");
                    return Ulong(-1);
                  }
                  user_executable_imports.env.__indirect_function_table.set(Number(slot), null);
"""
SIGLOOK_BATCH_OLD = """                const instance = new WebAssembly.Instance(module, imports);
"""
SIGLOOK_BATCH_NEW = """                // (A batch's slots follow it in memory: none of them may be one of the program's own.)
                if (Number(slot) <= -16) {
                  const slots = new DataView(memory.buffer, start + Number(len), 4 * (-16 - Number(slot)));
                  for (let i = 0; i < -16 - Number(slot); i++) {
                    if (slots.getUint32(4 * i, true) < program_slots && !sigfix_off) {
                      log("__rv_jit_install error: asked to put a region into slot " + slots.getUint32(4 * i, true) +
                          ", one of the program's own: refused (18)");
                      return Ulong(-1);
                    }
                  }
                }
                const instance = new WebAssembly.Instance(module, imports);
"""
SIGLOOK_INSTANCE_OLD = """          user_executable_instance = instance;
"""
SIGLOOK_INSTANCE_NEW = """          user_executable_instance = instance;
          program_slots = user_executable_imports.env.__indirect_function_table.length;
"""
SIGLOOK_SINGLE_OLD = """                if (index < 0) {
                  index = table.grow(1);
                }
"""
SIGLOOK_SINGLE_NEW = """                if (index < 0) {
                  index = table.grow(1);
                } else if (index < program_slots && !sigfix_off) {
                  log("__rv_jit_install error: asked to put a region into slot " + index + ", one of the program's own: refused (18)");
                  return Ulong(-1);
                }
"""


CLONEALIGN_OLD = """          if (instance.exports.__libc_clone_callback) {
            instance.exports.__libc_clone_callback();
"""
CLONEALIGN_NEW = """          if (instance.exports.__libc_clone_callback) {
            // (19) The function and its argument at the stack pointer, moved down so that what is left when
            // __libc_clone_callback() has taken them (16 bytes) is 16-byte aligned.
            if (arch_bits == 64) {
              const sp = user_executable_imports.env.__stack_pointer.value;
              const aligned = ((sp + 16n) & ~15n) - 16n;
              if (aligned != sp && !sigfix_off) {
                const view = new DataView(memory.buffer);
                const fn = view.getBigUint64(Number(sp), true), arg = view.getBigUint64(Number(sp) + 8, true);
                view.setBigUint64(Number(aligned), fn, true);
                view.setBigUint64(Number(aligned) + 8, arg, true);
                user_executable_imports.env.__stack_pointer.value = aligned;
              }
            }
            instance.exports.__libc_clone_callback();
"""


# 20. A trap of the program runs its handler.
TRAP_DECL_ANCHOR = "  /// Flag that a clone callback should be called instead of _start().\n"
TRAP_DECL = """  /// (20) What a trap of the program means as a signal: SIGFPE for arithmetic, SIGSEGV for a bad address (of data, of
  /// a function, the stack), SIGILL for the rest (unreachable, an error of the runtime).
  const trap_signal = (error) => {
    const text = String(error && error.message !== undefined ? error.message : error);
    if (error instanceof WebAssembly.RuntimeError &&
        /divide by zero|remainder by zero|divide result unrepresentable|float unrepresentable|integer overflow|invalid conversion to integer/.test(text)) {
      return 8;
    }
    if (error instanceof RangeError ||
        (error instanceof WebAssembly.RuntimeError &&
         /out of bounds|null function|signature mismatch|null pointer|invalid index into function table/.test(text))) {
      return 11;
    }
    return 4;
  };
  /// (20) How many traps in a row, a handler's own included: past a few, no handler is run any more.
  let fault_depth = 0;
  /// (22) A signal the kernel delivered at the task's first return to its program, before the program was there.
  let signal_at_start = false;

"""
TRAP_OLD = """          vmlinux_instance.exports.raise_exception();
          throw new Error("raise_exception() returned");
"""
TRAP_NEW = """          // (20) A trap of the program: the kernel makes a signal of it, whose handler runs on the way out of
          // wasm_user_fault, as at the return of a system call. A trap cannot be resumed: if the handler returns,
          // the program dies by the signal (-sig). Without the program's instance (it did not get to run) no
          // handler can run, nor in handler after handler that traps itself.
          const exports = vmlinux_instance.exports;
          if (exports.wasm_user_fault) {
            const sig = trap_signal(error);
            const sp = user_executable_imports ? user_executable_imports.env.__stack_pointer.value : exports.get_user_stack_pointer();
            const tls = user_executable_instance && user_executable_instance.exports.__get_tls_base ?
              user_executable_instance.exports.__get_tls_base() : exports.get_user_tls_base();
            const handler = user_executable_instance && fault_depth < 4;
            fault_depth++;
            try {
              if (handler) {
                exports.wasm_user_fault(sp, tls, Ulong(sig));
              }
              exports.wasm_user_fault(sp, tls, Ulong(-sig));
            } catch (next) {
              // The handler called exec (a Trap), or trapped itself (a trap again: its own signal is blocked there).
              return user_executable_error(next);
            }
            log("the kernel did not end a program that trapped (signal " + sig + ")");
            return;
          }
          vmlinux_instance.exports.raise_exception();
          throw new Error("raise_exception() returned");
"""

# 21. Ending a program that never comes into the kernel.
END_WORKER_HOOK_OLD = """    wasm_release_task: (dead_task) => {
"""
END_WORKER_HOOK_NEW = """    wasm_release_task: (dead_task) => {
      // (21) An odd address: not a release, but a fatal signal for a task running its program (the task's address
      // + 1): the main thread ends that task's Worker and runs the task's exit in a new one.
      if (dead_task & 1) {
        const task = dead_task - 1;
        const exports = vmlinux_instance.exports;
        if (exports.wasm_task_user_state) {
          port.postMessage({ method: "end_user_task", task: task, state: Number(exports.wasm_task_user_state(Ulong(task))) });
        }
        return;
      }
"""
END_WORKER_RUN_OLD = """        } else if (message.runner_type == "task") {
"""
END_WORKER_RUN_NEW = """        } else if (message.runner_type == "task_end") {
          // (21) A task whose Worker the main thread ended: become it, and take it to its exit (does not return).
          vmlinux_instance.exports.wasm_user_task_end(Ulong(message.task));
          throw new Error("wasm_user_task_end returned");
        } else if (message.runner_type == "task") {
"""
END_MAIN_OLD = """    release_task: (message) => {
"""
END_MAIN_NEW = """    end_user_task: (message) => {
      // (21) A fatal signal for a task that runs its program, which never comes into the kernel by itself. Its state
      // word goes from KILL (2) to ENDING (3) here, or the task has entered the kernel meanwhile (and takes the signal
      // there). Then its Worker is ended, and a new one runs the task to its exit (wasm_user_task_end).
      const word = new Int32Array(memory.buffer, Number(message.state), 1);
      if (Atomics.compareExchange(word, 0, 2, 3) != 2) {
        return;
      }
      switch_note("K", message.task, 2);
      const old = tasks[message.task];
      if (old) {
        old.worker.terminate();
      }
      const name = runner_names[message.task] || ("task " + message.task);
      log("[Main]: " + name + " has a fatal signal and runs its program: its Worker is ended, and a new one takes the task to its exit");
      tasks[message.task] = make_vmlinux_runner(name + " (ending)", { runner_type: "task_end", task: message.task });
    },

    release_task: (message) => {
"""

# 22. A signal at a new task's first return to its program.
START_SIG_OLD = """      } else if (flow >= 1 && flow <= 3) {
"""
START_SIG_NEW = """      } else if (flow >= 1 && flow <= 3) {
        // (22) A new task's first return to its program (ret_from_fork), with a signal to deliver, comes before
        // the program is instantiated: the handler runs when it is (user_executable_run).
        if (flow == 1 && !user_executable_instance) {
          signal_at_start = true;
          return;
        }
"""
START_RUN_OLD = """      const user_executable_run = (instance) => {
"""
START_RUN_NEW = """      const user_executable_run = (instance) => {
        if (signal_at_start) {
          // (22) The signal the kernel delivered before the program was there: its handler, before anything else.
          signal_at_start = false;
          host_callbacks.wasm_user_mode_tail(1);
        }
"""

def patch_file(path, name, marker, edits):
    """Apply (anchor, replacement) edits to a file unless `marker` shows it was done before."""
    with open(path) as f:
        text = f.read()
    if marker in text:
        print(name + ": already patched")
        return
    for anchor, replacement in edits:
        if anchor not in text:
            sys.exit(name + ": cannot find the place to patch in " + path + "; has it changed?")
        text = text.replace(anchor, replacement, 1)
    with open(path, "w") as f:
        f.write(text)
    print(name + ": patched")


# The hook as it was before regions imported the function table.
HOOK_V1_IMPORTS = "                const imports = { env: { memory: memory }, rv: {} };\n"
HOOK_V2_IMPORTS = """                // The table: regions jump to one another through it (tail calls).
                const imports = {
                  env: { memory: memory, table: user_executable_imports.env.__indirect_function_table },
                  rv: {},
                };
"""


JITCODE_WORKER_DECL_ANCHOR = "  let doorbell = null;\n"
JITCODE_WORKER_DECL = """  // The account of translated code (16): the page's array, and which cell of it is this Worker's (-1: none).
  let jit_code = null;
  let jit_cell = -1;
  // What each slot of the program's table holds, as the account has it (KiB).
  const jit_sizes = new Map();
  // What was let go of since the engine was last nudged to collect it (KiB).
  let jit_released = 0;
  const jit_account = (slot, kib) => {
    const before = jit_sizes.get(slot) || 0;
    if (kib) {
      jit_sizes.set(slot, kib);
    } else {
      jit_sizes.delete(slot);
    }
    if (jit_code && jit_cell >= 0 && kib != before) {
      // (The cell first: a Worker ended between the two leaves the sum short, never over.)
      Atomics.add(jit_code, 2 + jit_cell, kib - before);
      Atomics.add(jit_code, 1, kib - before);
    }
    // A module let go of (its slot emptied, or given another module: a batch's, say) is still there until the
    // engine collects garbage in this Worker, which it does when JavaScript here has allocated enough, and that
    // is little: the program is WebAssembly. So once 16 MiB worth of modules are no one's, an allocation that
    // counts: a large buffer, not touched, gone at once. (Measured under Node, 20000 modules one after another
    // in one slot: 48 MiB of code left, 8.5 with this.)
    if (before) {
      jit_released += before;
      if (jit_released >= 16384) {
        jit_released = 0;
        try {
          new ArrayBuffer(64 << 20);
        } catch (error) {
        }
      }
    }
  };
  // A new program (exec): the old one's table, and its modules, are no longer anyone's.
  const jit_account_reset = () => {
    for (const slot of [...jit_sizes.keys()]) {
      jit_account(slot, 0);
    }
  };
"""
JITCODE_WORKER_INIT_OLD = "      doorbell = message.doorbell || null;\n"
JITCODE_WORKER_INIT_NEW = """      doorbell = message.doorbell || null;
      jit_code = message.jit_code || null;
      jit_cell = message.jit_cell === undefined ? -1 : message.jit_cell;
"""
JITCODE_WORKER_SETUP_OLD = "      const user_executable_setup = () => {\n"
JITCODE_WORKER_SETUP_NEW = """      const user_executable_setup = () => {
        jit_account_reset();
"""
JITCODE_WORKER_HOOK_OLD = """            __rv_jit_install: (ptr, len, slot) => {
              try {
"""
JITCODE_WORKER_HOOK_NEW = """            __rv_jit_install: (ptr, len, slot) => {
              try {
                // A length of 0 (16): with slot -2, the budget for translated code and what all Workers have,
                // in KiB, as budget << 32 | sum (0 << 32: no budget); with a slot, empty it.
                if (Number(len) == 0) {
                  if (Number(slot) == -2) {
                    const budget = jit_code ? Math.max(0, Atomics.load(jit_code, 0)) : 0;
                    const sum = jit_code ? Math.max(0, Atomics.load(jit_code, 1)) : 0;
                    return Ulong((BigInt(budget) << 32n) | BigInt(Math.min(sum, 0xffffffff)));
                  }
                  user_executable_imports.env.__indirect_function_table.set(Number(slot), null);
                  jit_account(Number(slot), 0);
                  return Ulong(slot);
                }
"""
JITCODE_WORKER_SET_OLD = """                const instance = new WebAssembly.Instance(module, imports);
                const table = user_executable_imports.env.__indirect_function_table;
                let index = Number(slot);
"""
JITCODE_WORKER_SET_NEW = """                const instance = new WebAssembly.Instance(module, imports);
                const table = user_executable_imports.env.__indirect_function_table;
                // What a module takes of the engine's code space, about: three and a half times its length
                // (measured with rv's regions as made on the machine: Liftoff's code is 3.1 to 3.9 times as long
                // as the module, TurboFan's 3.5) and its jump tables, in pages of 16 KiB.
                const kib = Math.ceil((Number(len) * 7 / 2 + 2048) / 16384) * 16;
                // A batch (slot -16 - count): the module has put its functions into their slots itself, by its
                // element segments, when it was instantiated; the slots follow the module in memory.
                if (Number(slot) <= -16) {
                  const count = -16 - Number(slot);
                  const slots = new DataView(memory.buffer, start + Number(len), 4 * count);
                  for (let i = 0; i < count; i++) {
                    jit_account(slots.getUint32(4 * i, true), Math.ceil(kib / count));
                  }
                  return Ulong(0);
                }
                let index = Number(slot);
"""
JITCODE_WORKER_ACCOUNT_OLD = """                table.set(index, instance.exports.r);
                return Ulong(index);
"""
JITCODE_WORKER_ACCOUNT_NEW = """                table.set(index, instance.exports.r);
                jit_account(index, kib);
                return Ulong(index);
"""

JITCODE_MAIN_DECL_ANCHOR = "  const doorbell = new BigUint64Array(new SharedArrayBuffer(8));\n"
JITCODE_MAIN_DECL = """  /// The account of translated code (16): [0] the budget in KiB, [1] the sum of the cells, then a cell for each
  /// Worker that has one. A cell is handed out with the Worker and given back, zeroed, when the Worker is ended.
  const JIT_CELLS = 8192;
  const jit_code = new Int32Array(new SharedArrayBuffer(4 * (2 + JIT_CELLS)));
  {
    const query = typeof location != "undefined" ? location.search : (globalThis.rv_query || "");
    const asked = /[?&]jitcode=([0-9]+)(&|$)/.exec(query);
    jit_code[0] = (asked ? Number(asked[1]) : 1536) * 1024;
  }
  const jit_cells_free = [];
  for (let i = JIT_CELLS - 1; i >= 0; i--) {
    jit_cells_free.push(i);
  }
"""
# (A runtime patched when the budget was 512 MiB by default.)
JITCODE_BUDGET_512 = "jit_code[0] = (asked ? Number(asked[1]) : 512) * 1024;"
JITCODE_BUDGET_1536 = "jit_code[0] = (asked ? Number(asked[1]) : 1536) * 1024;"
JITCODE_MAIN_WORKER_OLD = "    const worker = new Worker(worker_url, { name: name });\n"
JITCODE_MAIN_WORKER_NEW = """    const worker = new Worker(worker_url, { name: name });
    // Its cell in the account of translated code, given back when the Worker is ended (from wherever that is).
    const jit_cell = jit_cells_free.length ? jit_cells_free.pop() : -1;
    if (jit_cell >= 0) {
      const terminate = worker.terminate.bind(worker);
      let ended = false;
      worker.terminate = () => {
        if (!ended) {
          ended = true;
          Atomics.sub(jit_code, 1, Atomics.exchange(jit_code, 2 + jit_cell, 0));
          jit_cells_free.push(jit_cell);
        }
        return terminate();
      };
    }
"""
JITCODE_MAIN_INIT_OLD = "      doorbell: doorbell,\n"
JITCODE_MAIN_INIT_NEW = """      doorbell: doorbell,
      jit_code: jit_code,
      jit_cell: jit_cell,
"""
JITCODE_MAIN_RETURN_OLD = "    get switches() { return switches_all(); },\n"
JITCODE_MAIN_RETURN_NEW = """    get switches() { return switches_all(); },
    // Translated code as the account has it (16), in KiB: the budget, the sum, and the Workers that have some.
    get jit_code() {
      let workers = 0;
      for (let i = 0; i < JIT_CELLS; i++) {
        workers += Atomics.load(jit_code, 2 + i) > 0 ? 1 : 0;
      }
      return { budget: Atomics.load(jit_code, 0), sum: Atomics.load(jit_code, 1), workers: workers };
    },
"""

CONSOLE_IN_WORKER_DECL_ANCHOR = "  let doorbell = null;\n"
CONSOLE_IN_WORKER_DECL = """  /// The console's input (17): a ring of bytes the main thread fills and the console driver empties, or null.
  let console_in = null;
"""
CONSOLE_IN_WORKER_INIT_OLD = "      doorbell = message.doorbell || null;\n"
CONSOLE_IN_WORKER_INIT_NEW = """      doorbell = message.doorbell || null;
      console_in = message.console_in || null;
"""
CONSOLE_IN_WORKER_GET_OLD = "    wasm_driver_hvc_get: (buffer, count) => {\n"
CONSOLE_IN_WORKER_GET_NEW = """    wasm_driver_hvc_get: (buffer, count) => {
      // Straight from the ring (17): the main thread is not asked, and this CPU does not wait for it.
      if (console_in) {
        const size = console_in.bytes.length;
        const read = Atomics.load(console_in.index, 1);
        const waiting = (Atomics.load(console_in.index, 0) - read) | 0;
        const n = Math.max(0, Math.min(waiting, count));
        const at = read & (size - 1);
        const first = Math.min(n, size - at);
        const memory_u8 = new Uint8Array(memory.buffer);
        memory_u8.set(console_in.bytes.subarray(at, at + first), buffer);
        memory_u8.set(console_in.bytes.subarray(0, n - first), buffer + first);
        Atomics.store(console_in.index, 1, (read + n) | 0);
        if (n > 0 && waiting >= size / 2) {
          port.postMessage({ method: "console_room" });  // (more may be waiting on the main thread)
        }
        return n;
      }

"""

CONSOLE_IN_MAIN_DECL_ANCHOR = "  const doorbell = new BigUint64Array(new SharedArrayBuffer(8));\n"
CONSOLE_IN_MAIN_DECL = """  /// The console's input (17): typed bytes wait in a ring in shared memory until the kernel's console driver
  /// takes them; index[0] counts the bytes put in, index[1] those taken out. What does not fit waits in
  /// input_buffer, and goes in when there is room (the next key, or the driver says it has made room).
  const console_in = {
    bytes: new Uint8Array(new SharedArrayBuffer(1 << 20)),
    index: new Int32Array(new SharedArrayBuffer(8)),
  };
  const console_in_fill = () => {
    const size = console_in.bytes.length;
    const written = Atomics.load(console_in.index, 0);
    const room = size - ((written - Atomics.load(console_in.index, 1)) | 0);
    const pending = new Uint8Array(input_buffer);
    const n = Math.min(room, pending.length);
    if (n <= 0) {
      return;
    }
    const at = written & (size - 1);
    const first = Math.min(n, size - at);
    console_in.bytes.set(pending.subarray(0, first), at);
    console_in.bytes.set(pending.subarray(first, n), 0);
    input_buffer = pending.slice(n).buffer;
    Atomics.store(console_in.index, 0, (written + n) | 0);
  };

"""
CONSOLE_IN_MAIN_INIT_OLD = "      doorbell: doorbell,\n"
CONSOLE_IN_MAIN_INIT_NEW = """      doorbell: doorbell,
      console_in: console_in,
"""
CONSOLE_IN_MAIN_CALLBACK_ANCHOR = "    console_write: (message) => {\n"
CONSOLE_IN_MAIN_CALLBACK = """    console_room: () => {
      console_in_fill();
    },

"""
CONSOLE_IN_MAIN_KEY_OLD = "      (new Uint8Array(input_buffer)).set(key_buffer, old_size);\n"
CONSOLE_IN_MAIN_KEY_NEW = """      (new Uint8Array(input_buffer)).set(key_buffer, old_size);
      console_in_fill();
"""


TABLE_LOAD_OLD = """      user_executable = WebAssembly.compile(new Uint8Array(memory.buffer).slice(bin_start, bin_end));
      user_executable_params = {
        data_start: data_start,
        table_start: table_start,
      };
"""
TABLE_LOAD_NEW = """      const user_bytes = new Uint8Array(memory.buffer).slice(bin_start, bin_end);
      user_executable = WebAssembly.compile(user_bytes);
      user_executable_params = {
        data_start: data_start,
        table_start: table_start,
        table_min: wasm_table_min(user_bytes),
      };
"""
TABLE_DECL_ANCHOR = "  let user_executable_params = null;\n"
TABLE_DECL = """  // The size the program's import of its function table asks for (0: none), from its import section (23).
  const wasm_table_min = (bytes) => {
    let p = 8;
    const leb = () => {
      let result = 0, shift = 0, byte;
      do {
        byte = bytes[p++];
        result += (byte & 0x7f) * 2 ** shift;
        shift += 7;
      } while (byte & 0x80);
      return result;
    };
    const skip_limits = () => {
      const flags = bytes[p++];
      const min = leb();
      if (flags & 1) leb();
      return min;
    };
    try {
      while (p < bytes.length) {
        const id = bytes[p++];
        const size = leb();
        const end = p + size;
        if (id == 2) {
          for (let count = leb(); count > 0; count--) {
            for (let name = 0; name < 2; name++) {
              const length = leb();
              p += length;
            }
            const kind = bytes[p++];
            if (kind == 0) leb();
            else if (kind == 1) { p++; return skip_limits(); }
            else if (kind == 2) skip_limits();
            else if (kind == 3) p += 2;
            else if (kind == 4) { p++; leb(); }
          }
          return 0;
        }
        if (id > 2 && id != 0) return 0;
        p = end;
      }
    } catch (error) {
    }
    return 0;
  };
"""
RELOCS_OLD = """          instance.exports.__wasm_apply_data_relocs();
          if (should_call_clone_callback) {"""
RELOCS_NEW = """          // A thread shares the memory of a program whose pointers were relocated when it started, and may
          // have changed since: applying the relocations again would put their first values back (24).
          if (!should_call_clone_callback) {
            instance.exports.__wasm_apply_data_relocs();
          }
          if (should_call_clone_callback) {"""
TABLE_SETUP_OLD = """__indirect_function_table: new WebAssembly.Table({ initial: 4096, element: "anyfunc" }), // TODO: fix this!"""
TABLE_SETUP_NEW = """__indirect_function_table: new WebAssembly.Table({
              initial: Math.max(4096, user_executable_params.table_min || 0), element: "anyfunc" }),"""
AUDIO_WORKER_DECL_ANCHOR = "  /// Tell the main thread where the kernel takes the interrupts the host raises (see linux.js: raise_irq).\n"
AUDIO_WORKER_DECL = """  /// (25) Where the main thread finds the sound card's header (see doorbell_advertise), or null.
  let audio_where = null;

"""
AUDIO_WORKER_ADVERTISE_OLD = """      Atomics.store(doorbell, 0, BigInt(vmlinux_instance.exports.wasm_irq_cpu_word()));
    }
  };
"""
AUDIO_WORKER_ADVERTISE_NEW = """      Atomics.store(doorbell, 0, BigInt(vmlinux_instance.exports.wasm_irq_cpu_word()));
    }
    // (25) And where the sound card's header is (sound/drivers/wasm_audio.c), for the page's player.
    if (audio_where && !Atomics.load(audio_where, 0) && vmlinux_instance && vmlinux_instance.exports.wasm_audio_header) {
      Atomics.store(audio_where, 0, BigInt(vmlinux_instance.exports.wasm_audio_header()));
    }
  };
"""
AUDIO_WORKER_INIT_OLD = "      doorbell = message.doorbell || null;\n"
AUDIO_WORKER_INIT_NEW = """      doorbell = message.doorbell || null;
      audio_where = message.audio_where || null;
"""
AUDIO_MAIN_DECL_ANCHOR = "  const doorbell = new BigUint64Array(new SharedArrayBuffer(8));\n"
AUDIO_MAIN_DECL = """  /// (25) Where the sound card's header is (sound/drivers/wasm_audio.c: wasm_audio_header()), as a Worker found it;
  /// 0 until then, and for good with a kernel without the card. The page's player reads it through os.audio.
  const audio_where = new BigUint64Array(new SharedArrayBuffer(8));
"""
AUDIO_MAIN_INIT_OLD = "      doorbell: doorbell,\n"
AUDIO_MAIN_INIT_NEW = """      doorbell: doorbell,
      audio_where: audio_where,
"""
AUDIO_MAIN_RETURN_ANCHOR = "    runner_names: runner_names,\n"
AUDIO_MAIN_RETURN = """    /// (25) The sound card, for the page's player (tools/www/audio.js): where its header is in the memory, and the
    /// word in which the host raises the kernel's interrupts (WASM_IRQ_AUDIO is one); 0 while not known.
    audio: {
      header: () => Number(Atomics.load(audio_where, 0)),
      irq_word: () => Number(Atomics.load(doorbell, 0)),
    },
"""


def main():
    runtime = sys.argv[1]
    worker = runtime + "/linux-worker.js"
    patch_file(worker, "JIT hook", "__rv_jit_install", [(ANCHOR, HOOK.lstrip("\n") + "\n" + ANCHOR)])
    patch_file(worker, "JIT hook (function table)", "table: user_executable_imports",
               [(HOOK_V1_IMPORTS, HOOK_V2_IMPORTS)])
    patch_file(worker, "spare runners (worker)", '"spare"',
               [(SPARE_WORKER_ANCHOR, SPARE_WORKER + SPARE_WORKER_ANCHOR)])
    patch_file(worker, "network (worker)", "wasm_driver_net_tx",
               [(NET_WORKER_DECL_ANCHOR, NET_WORKER_DECL), (NET_WORKER_INIT_ANCHOR, NET_WORKER_INIT),
                (NET_WORKER_ANCHOR, NET_WORKER + NET_WORKER_ANCHOR)])
    patch_file(runtime + "/linux.js", "network (main)", "net_socket",
               [(NET_MAIN_SIGNATURE_OLD, NET_MAIN_SIGNATURE_NEW), (NET_MAIN_SETUP_ANCHOR, NET_MAIN_SETUP),
                (NET_MAIN_CALLBACK_ANCHOR, NET_MAIN_CALLBACK), (NET_MAIN_INIT_ANCHOR, NET_MAIN_INIT)])
    patch_file(worker, "display and input (worker)", "wasm_driver_fb_show",
               [(GUI_WORKER_DECL_ANCHOR, GUI_WORKER_DECL + GUI_WORKER_DECL_ANCHOR),
                (GUI_WORKER_INIT_ANCHOR, GUI_WORKER_INIT),
                (GUI_WORKER_ANCHOR, GUI_WORKER + GUI_WORKER_ANCHOR)])
    patch_file(runtime + "/linux.js", "display and input (main)", "input_event",
               [(GUI_MAIN_SIGNATURE_OLD, GUI_MAIN_SIGNATURE_NEW),
                (GUI_MAIN_SETUP_ANCHOR, GUI_MAIN_SETUP + GUI_MAIN_SETUP_ANCHOR),
                (GUI_MAIN_CALLBACK_ANCHOR, GUI_MAIN_CALLBACK + GUI_MAIN_CALLBACK_ANCHOR),
                (GUI_MAIN_INIT_ANCHOR, GUI_MAIN_INIT), (GUI_MAIN_RETURN_ANCHOR, GUI_MAIN_RETURN)])
    patch_file(worker, "disk (worker)", "wasm_driver_blk_rw",
               [(DISK_WORKER_DECL_ANCHOR, DISK_WORKER_DECL + DISK_WORKER_DECL_ANCHOR),
                (DISK_WORKER_INIT_ANCHOR, DISK_WORKER_INIT),
                (DISK_WORKER_ANCHOR, DISK_WORKER + DISK_WORKER_ANCHOR)])
    patch_file(runtime + "/linux.js", "disk (main)", "disk_options",
               [(DISK_MAIN_SIGNATURE_OLD, DISK_MAIN_SIGNATURE_NEW),
                (DISK_MAIN_DECL_ANCHOR, DISK_MAIN_DECL + DISK_MAIN_DECL_ANCHOR),
                (DISK_MAIN_SETUP_ANCHOR, DISK_MAIN_SETUP + DISK_MAIN_SETUP_ANCHOR),
                (DISK_MAIN_INIT_ANCHOR, DISK_MAIN_INIT)])
    patch_file(runtime + "/linux.js", "spare runners (main)", "spare_runners",
               [(SPARE_MAIN_DECL_ANCHOR, SPARE_MAIN_DECL + SPARE_MAIN_DECL_ANCHOR),
                (SPARE_MAIN_OLD, SPARE_MAIN_NEW)])

    patch_file(worker, "crash with a stack trace", "its \"name\" section",
               [(CRASH_WORKER_OLD, CRASH_WORKER_NEW)])
    patch_file(worker, "one clock for all Workers", "let correction = 0;",
               [(CLOCK_WORKER_OLD, CLOCK_WORKER_NEW)])
    patch_file(runtime + "/linux.js", "the book of runners", "switch_note",
               [(BOOK_MAIN_DECL_ANCHOR, BOOK_MAIN_DECL + BOOK_MAIN_DECL_ANCHOR),
                (BOOK_MAIN_START_OLD, BOOK_MAIN_START_NEW), (BOOK_MAIN_STOP_OLD, BOOK_MAIN_STOP_NEW),
                (BOOK_MAIN_CREATE_OLD, BOOK_MAIN_CREATE_NEW), (BOOK_MAIN_CPU_OLD, BOOK_MAIN_CPU_NEW),
                (BOOK_MAIN_TASK_OLD, BOOK_MAIN_TASK_NEW), (BOOK_MAIN_KILL_OLD, BOOK_MAIN_KILL_NEW),
                (BOOK_MAIN_RETURN_OLD, BOOK_MAIN_RETURN_NEW)])
    patch_file(worker, "signal handlers one at a time", "signals_old",
               [(SIGNALS_WORKER_DECL_ANCHOR, SIGNALS_WORKER_DECL), (SIGNALS_WORKER_INIT_ANCHOR, SIGNALS_WORKER_INIT),
                (SIGNALS_WORKER_OLD, SIGNALS_WORKER_NEW)])
    patch_file(runtime + "/linux.js", "signal handlers one at a time (main)", "signals_old",
               [(SIGNALS_MAIN_OLD, SIGNALS_MAIN_NEW)])
    patch_file(worker, "a crash inside the kernel", "wasm_cpuflags_now", [(CRASH_KERNEL_OLD, CRASH_KERNEL_NEW)])
    patch_file(runtime + "/linux.js", "a crash inside the kernel (main)", "kernel_crashes",
               [(CRASH_KERNEL_MAIN_DECL_ANCHOR, CRASH_KERNEL_MAIN_DECL + CRASH_KERNEL_MAIN_DECL_ANCHOR),
                (CRASH_KERNEL_MAIN_ANCHOR, CRASH_KERNEL_MAIN + CRASH_KERNEL_MAIN_ANCHOR),
                (CRASH_KERNEL_MAIN_RETURN_OLD, CRASH_KERNEL_MAIN_RETURN_NEW)])

    patch_file(worker, "JavaScript as a program", "__run_js",
               [(JS_WORKER_DECL_ANCHOR, JS_WORKER_DECL + JS_WORKER_DECL_ANCHOR),
                (JS_WORKER_PORT_OLD, JS_WORKER_PORT_NEW),
                (JS_WORKER_CLOCK_OLD, JS_WORKER_CLOCK_NEW),
                (JS_WORKER_RANDOM_OLD, JS_WORKER_RANDOM_NEW),
                (ANCHOR, JS_WORKER_IMPORT + ANCHOR),
                (JS_WORKER_RUN_ANCHOR, JS_WORKER_RUN + JS_WORKER_RUN_ANCHOR),
                (JS_WORKER_TRAP_ANCHOR, JS_WORKER_TRAP + JS_WORKER_TRAP_ANCHOR)])

    patch_file(worker, "task switches without the main thread", "switch_area",
               [(SWITCH_WORKER_DECL_ANCHOR, SWITCH_WORKER_DECL + SWITCH_WORKER_DECL_ANCHOR),
                (SWITCH_WORKER_HELPERS_ANCHOR, SWITCH_WORKER_HELPERS + SWITCH_WORKER_HELPERS_ANCHOR),
                (SWITCH_WORKER_CREATE_POST_OLD, SWITCH_WORKER_CREATE_POST_NEW),
                (SWITCH_WORKER_CREATE_OLD, SWITCH_WORKER_CREATE_NEW),
                (SWITCH_WORKER_SERIALIZE_OLD, SWITCH_WORKER_SERIALIZE_NEW),
                (SWITCH_WORKER_SERIALIZE_TAIL_OLD, SWITCH_WORKER_SERIALIZE_TAIL_NEW),
                (SWITCH_WORKER_SERIALIZE_ME_OLD, SWITCH_WORKER_SERIALIZE_ME_NEW),
                (SWITCH_WORKER_INIT_ANCHOR, SWITCH_WORKER_INIT),
                (DOORBELL_WORKER_INPUT_OLD, DOORBELL_WORKER_INPUT_NEW),
                (DOORBELL_WORKER_NET_OLD, DOORBELL_WORKER_NET_NEW)])
    patch_file(runtime + "/linux.js", "task switches without the main thread (main)", "switches_all",
               [(SWITCH_MAIN_DECL_ANCHOR, SWITCH_MAIN_DECL + SWITCH_MAIN_DECL_ANCHOR),
                (SWITCH_MAIN_CREATE_OLD, SWITCH_MAIN_CREATE_NEW),
                (SWITCH_MAIN_DONE_ANCHOR, SWITCH_MAIN_DONE + SWITCH_MAIN_DONE_ANCHOR),
                (SWITCH_MAIN_INIT_ANCHOR, SWITCH_MAIN_INIT + SWITCH_MAIN_INIT_ANCHOR),
                (SWITCH_MAIN_RETURN_OLD, SWITCH_MAIN_RETURN_NEW),
                (DOORBELL_MAIN_INPUT_OLD, DOORBELL_MAIN_INPUT_NEW),
                (DOORBELL_MAIN_NET_OLD, DOORBELL_MAIN_NET_NEW)])

    patch_file(worker, "waits that a stale wake cannot end", "a late notify of an older wake",
               [(STALE_WORKER_LOCK_OLD, STALE_WORKER_LOCK_NEW), (STALE_WORKER_HVC_OLD, STALE_WORKER_HVC_NEW)])
    patch_file(runtime + "/linux.js", "waits that a stale wake cannot end (main)", "Atomics.exchange(locks._memory",
               [(STALE_MAIN_LOCK_OLD, STALE_MAIN_LOCK_NEW)])
    patch_file(runtime + "/linux.js", "a memory as large as the machine's", "kV8MaxWasmMemory64Pages",
               [(MEMORY_MAX_OLD, MEMORY_MAX_NEW)])

    patch_file(worker, "a bound on translated code", "jit_account_reset",
               [(JITCODE_WORKER_DECL_ANCHOR, JITCODE_WORKER_DECL + JITCODE_WORKER_DECL_ANCHOR),
                (JITCODE_WORKER_INIT_OLD, JITCODE_WORKER_INIT_NEW),
                (JITCODE_WORKER_SETUP_OLD, JITCODE_WORKER_SETUP_NEW),
                (JITCODE_WORKER_HOOK_OLD, JITCODE_WORKER_HOOK_NEW),
                (JITCODE_WORKER_SET_OLD, JITCODE_WORKER_SET_NEW),
                (JITCODE_WORKER_ACCOUNT_OLD, JITCODE_WORKER_ACCOUNT_NEW)])
    patch_file(runtime + "/linux.js", "a bound on translated code (main)", "jit_cells_free",
               [(JITCODE_MAIN_DECL_ANCHOR, JITCODE_MAIN_DECL + JITCODE_MAIN_DECL_ANCHOR),
                (JITCODE_MAIN_WORKER_OLD, JITCODE_MAIN_WORKER_NEW),
                (JITCODE_MAIN_INIT_OLD, JITCODE_MAIN_INIT_NEW),
                (JITCODE_MAIN_RETURN_OLD, JITCODE_MAIN_RETURN_NEW)])
    patch_file(runtime + "/linux.js", "a bound on translated code: 1536 MiB by default", ": 1536) * 1024",
               [(JITCODE_BUDGET_512, JITCODE_BUDGET_1536)])

    patch_file(worker, "the console's input without the main thread", "console_in",
               [(CONSOLE_IN_WORKER_DECL_ANCHOR, CONSOLE_IN_WORKER_DECL + CONSOLE_IN_WORKER_DECL_ANCHOR),
                (CONSOLE_IN_WORKER_INIT_OLD, CONSOLE_IN_WORKER_INIT_NEW),
                (CONSOLE_IN_WORKER_GET_OLD, CONSOLE_IN_WORKER_GET_NEW)])
    patch_file(runtime + "/linux.js", "the console's input without the main thread (main)", "console_in_fill",
               [(CONSOLE_IN_MAIN_DECL_ANCHOR, CONSOLE_IN_MAIN_DECL + CONSOLE_IN_MAIN_DECL_ANCHOR),
                (CONSOLE_IN_MAIN_INIT_OLD, CONSOLE_IN_MAIN_INIT_NEW),
                (CONSOLE_IN_MAIN_CALLBACK_ANCHOR, CONSOLE_IN_MAIN_CALLBACK + CONSOLE_IN_MAIN_CALLBACK_ANCHOR),
                (CONSOLE_IN_MAIN_KEY_OLD, CONSOLE_IN_MAIN_KEY_NEW)])


    patch_file(worker, "signals looked at before their handler is called", "programs_loaded",
               [(SIGLOOK_DECL_ANCHOR, SIGLOOK_DECL + SIGLOOK_DECL_ANCHOR),
                (SIGLOOK_LOAD_OLD, SIGLOOK_LOAD_NEW),
                (SIGLOOK_SPARE_OLD, SIGLOOK_SPARE_NEW),
                (SIGLOOK_HELPER_ANCHOR, SIGLOOK_HELPER + SIGLOOK_HELPER_ANCHOR),
                (SIGLOOK_CALL_OLD, SIGLOOK_CALL_NEW),
                (SIGLOOK_RELEASE_OLD, SIGLOOK_RELEASE_NEW),
                (SIGLOOK_BATCH_OLD, SIGLOOK_BATCH_NEW),
                (SIGLOOK_SINGLE_OLD, SIGLOOK_SINGLE_NEW),
                (SIGLOOK_INSTANCE_OLD, SIGLOOK_INSTANCE_NEW),
                (SIGLOOK_INIT_OLD, SIGLOOK_INIT_NEW)])
    patch_file(runtime + "/linux.js", "signals looked at before their handler is called (main)", "sigfix_off",
               [(SIGLOOK_MAIN_OLD, SIGLOOK_MAIN_NEW)])

    patch_file(worker, "a new thread's stack aligned to 16 bytes", "(19) The function and its argument",
               [(CLONEALIGN_OLD, CLONEALIGN_NEW)])

    patch_file(worker, "a trap of the program runs its handler", "(20) What a trap",
               [(TRAP_DECL_ANCHOR, TRAP_DECL + TRAP_DECL_ANCHOR), (TRAP_OLD, TRAP_NEW)])
    patch_file(worker, "ending a program that never comes into the kernel", "(21) An odd address",
               [(END_WORKER_HOOK_OLD, END_WORKER_HOOK_NEW),
                (END_WORKER_RUN_OLD, END_WORKER_RUN_NEW)])
    patch_file(runtime + "/linux.js", "ending a program that never comes into the kernel (main)", "end_user_task",
               [(END_MAIN_OLD, END_MAIN_NEW)])
    patch_file(worker, "a signal at a new task's first return to its program", "(22) A new task's first return",
               [(START_SIG_OLD, START_SIG_NEW), (START_RUN_OLD, START_RUN_NEW)])
    patch_file(worker, "a function table as large as the program asks for", "wasm_table_min",
               [(TABLE_DECL_ANCHOR, TABLE_DECL_ANCHOR + TABLE_DECL), (TABLE_LOAD_OLD, TABLE_LOAD_NEW),
                (TABLE_SETUP_OLD, TABLE_SETUP_NEW)])
    patch_file(worker, "threads keep the program's relocated pointers", "may\n          // have changed since",
               [(RELOCS_OLD, RELOCS_NEW)])

    patch_file(worker, "sound", "(25) Where the main thread finds the sound card's header",
               [(AUDIO_WORKER_DECL_ANCHOR, AUDIO_WORKER_DECL + AUDIO_WORKER_DECL_ANCHOR),
                (AUDIO_WORKER_ADVERTISE_OLD, AUDIO_WORKER_ADVERTISE_NEW),
                (AUDIO_WORKER_INIT_OLD, AUDIO_WORKER_INIT_NEW)])
    patch_file(runtime + "/linux.js", "sound (main)", "(25) Where the sound card's header is",
               [(AUDIO_MAIN_DECL_ANCHOR, AUDIO_MAIN_DECL + AUDIO_MAIN_DECL_ANCHOR),
                (AUDIO_MAIN_INIT_OLD, AUDIO_MAIN_INIT_NEW),
                (AUDIO_MAIN_RETURN_ANCHOR, AUDIO_MAIN_RETURN + AUDIO_MAIN_RETURN_ANCHOR)])

if __name__ == "__main__":
    main()
