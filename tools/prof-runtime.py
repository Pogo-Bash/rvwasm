#!/usr/bin/env python3
"""Make a copy of the Linux/Wasm web directory whose runtime records when each
step of starting a task happened. Run inside the build VM:

    tools/prof-runtime.py ~/rvwasm/www ~/rvwasm/www/prof

then use http://localhost:8000/prof/dev.html; the records collect in the
page's global __prof (one per run of a program: times in ms since the epoch).

  req      the parent asked for the task (in its worker)
  main     the main thread got the request and created the Worker
  script   the Worker's script started running
  init     the Worker got its "init" message
  vmlinux  the kernel was instantiated in the Worker
  assign   a spare Worker was given the task
  compile  [start, end, bytes] of compiling the program
  inst     [start, end] of instantiating it
  run      the program was entered
"""
import os
import sys

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)


def patch(text, old, new, count=1):
    if text.count(old) < 1:
        sys.exit("pattern not found: " + old[:60])
    return text.replace(old, new, count)


w = open(os.path.join(src, "linux-worker.js")).read()
w = patch(w, "  let port = self;\n",
          "  let port = self;\n"
          "  const __t = () => performance.timeOrigin + performance.now();\n"
          "  let __prof = { script: __t() };\n")
w = patch(w, '        method: "create_and_run_task",\n',
          '        method: "create_and_run_task",\n        t_req: __t(),\n')
w = patch(w, "      variant = message.variant;\n",
          "      __prof.init = __t(); Object.assign(__prof, message.prof || {}); __prof.name = message.runner_name;\n"
          "      variant = message.variant;\n")
w = patch(w, "          message = assignment;\n",
          "          message = assignment; Object.assign(__prof, message.prof || {}); __prof.assign = __t();\n")
w = patch(w, "          vmlinux_instance = instance;\n",
          "          vmlinux_instance = instance; __prof.vmlinux = __t();\n")
w = patch(w, "      user_executable = WebAssembly.compile(new Uint8Array(memory.buffer).slice(bin_start, bin_end));\n",
          "      const __c0 = __t();\n"
          "      user_executable = WebAssembly.compile(new Uint8Array(memory.buffer).slice(bin_start, bin_end));\n"
          "      user_executable.then(() => { __prof.compile = [__c0, __t(), bin_end - bin_start]; });\n")
w = patch(w, "        const stack_pointer = vmlinux_instance.exports.get_user_stack_pointer();\n",
          "        const __s0 = __t();\n"
          "        const stack_pointer = vmlinux_instance.exports.get_user_stack_pointer();\n")
w = patch(w, "          user_executable_instance = instance;\n          return instance;\n",
          "          user_executable_instance = instance; __prof.inst = [__s0, __t()];\n          return instance;\n")
w = patch(w, "      const user_executable_run = (instance) => {\n",
          "      const user_executable_run = (instance) => {\n"
          "        __prof.run = __t(); __prof.clone = should_call_clone_callback;\n"
          '        port.postMessage({ method: "prof", prof: __prof });\n'
          "        __prof = { name: __prof.name, exec: true };\n")
open(os.path.join(dst, "linux-worker.js"), "w").write(w)

m = open(os.path.join(src, "linux.js")).read()
m = patch(m, "    create_and_run_task: (message) => {\n",
          "    prof: (message) => { (globalThis.__prof ||= []).push(message.prof); },\n\n"
          "    create_and_run_task: (message) => {\n"
          "      globalThis.__req = { req: message.t_req, main: performance.timeOrigin + performance.now() };\n")
m = patch(m, "      user_executable: user_executable,\n    };\n",
          "      user_executable: user_executable,\n      prof: globalThis.__req,\n    };\n")
open(os.path.join(dst, "linux.js"), "w").write(m)

for name in os.listdir(src):
    if name in ("linux.js", "linux-worker.js") or os.path.isdir(os.path.join(src, name)):
        continue
    target = os.path.join(dst, name)
    if os.path.lexists(target):
        os.remove(target)
    os.symlink(os.path.realpath(os.path.join(src, name)), target)
