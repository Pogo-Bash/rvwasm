// Starting Node's own library.
//
// What node's C++ does before and around lib/: make the `process` object and
// `primordials`, run the bootstrap scripts in their order, answer
// internalBinding(), parse the command line, pick the main script, and at the
// end of the program send `beforeExit` and `exit`. Each step names the place
// in Node's source (v20) that it follows.
(function (hn) {
  "use strict";

  const { loop, web } = hn;

  // A pipe whose reader has gone is an error of the write, not the end of the program (node does the same).
  hn.C.hn_signal(13, 1);

  const data = JSON.parse(hn.readText(hn.home + "/node.json"));
  hn.data = data;

  // --- The global object. A Worker's has what a page's worker needs; programs take some of it for a sign
  // that they are in a browser, and act on it. What Node has under the same name, lib/ puts there itself.
  for (const name of ["self", "importScripts", "location", "navigator", "postMessage", "close", "name",
                      "caches", "indexedDB", "origin", "isSecureContext", "crossOriginIsolated",
                      "WorkerGlobalScope", "DedicatedWorkerGlobalScope", "WorkerNavigator", "WorkerLocation",
                      "XMLHttpRequest", "FileReaderSync", "createImageBitmap", "requestAnimationFrame",
                      "cancelAnimationFrame", "webkitRequestFileSystem", "webkitRequestFileSystemSync",
                      "webkitResolveLocalFileSystemURL", "webkitResolveLocalFileSystemSyncURL",
                      "onmessageerror", "onerror", "onlanguagechange", "onoffline", "ononline",
                      "onrejectionhandled", "onunhandledrejection", "reportError", "scheduler",
                      "trustedTypes", "fonts", "cookieStore", "EventSource", "WebSocket", "Worker",
                      "SharedWorker", "Notification", "FileReader", "IDBFactory", "OffscreenCanvas",
                      "ImageData", "ImageBitmap", "Path2D", "Cache", "CacheStorage", "FileSystemHandle",
                      "FileSystemFileHandle", "FileSystemDirectoryHandle", "FontFace", "PushManager",
                      "ServiceWorkerRegistration", "NavigationPreloadManager", "StorageManager"]) {
    // Wherever it is: the object itself, or what stands behind it.
    for (let o = globalThis; o && o !== Object.prototype; o = Object.getPrototypeOf(o)) {
      try {
        delete o[name];
      } catch (error) {
      }
    }
  }

  // --- WebAssembly for the program. Compiling "in the background" ends in a task of the browser's loop,
  // which does not come while our loop waits in the kernel (see loop.js); a worker may also compile while
  // it waits, of any size, so that is what the asynchronous calls do here.
  {
    const W = WebAssembly;
    const bytesOf = async (source) => {
      source = await source;
      if (source && typeof source.arrayBuffer === "function") {
        return source.arrayBuffer();	// a Response
      }
      return source;
    };
    W.compile = async (bytes) => new W.Module(await bytesOf(bytes));
    W.instantiate = async (source, imports) => {
      source = await bytesOf(source);
      if (source instanceof W.Module) {
        return new W.Instance(source, imports);
      }
      const module = new W.Module(source);
      return { module, instance: new W.Instance(module, imports) };
    };
    W.compileStreaming = W.compile;
    W.instantiateStreaming = W.instantiate;
  }

  // --- Symbols of node's that are private to it: a property under one of them is not found by looking at an
  // object's own keys. Here each is an ordinary symbol with an accessor on Object.prototype that keeps the
  // value aside, which gives the same: obj[symbol] = value makes no property.
  const privateSymbols = {};
  for (const name of Object.keys(data.bindings.util.privateSymbols)) {
    const symbol = Symbol(data.bindings.util.privateSymbols[name].description);
    const values = new WeakMap();
    Object.defineProperty(Object.prototype, symbol, {
      get() {
        return (typeof this === "object" && this !== null) || typeof this === "function" ? values.get(this) : undefined;
      },
      set(value) {
        if ((typeof this === "object" && this !== null) || typeof this === "function") {
          values.set(this, value);
        }
      },
      enumerable: false,
      configurable: false,
    });
    privateSymbols[name] = symbol;
  }
  hn.privateSymbols = privateSymbols;

  // --- The command line (src/node_options.cc, in brief: the options and their defaults are the real ones,
  // taken from the guest's node).
  const values = {};
  for (const key of Object.keys(data.options)) {
    const v = data.options[key];
    values[key] = v !== null && typeof v === "object" && v.$ === "undefined" ? undefined : v;
  }
  // (The extraction ran with these two.)
  values["--expose-internals"] = false;
  values["--no-expose-internals"] = true;
  values["--warnings"] = true;
  values["--no-warnings"] = false;
  const info = new Map(data.optionsInfo.options.entries);
  const aliases = new Map(data.optionsInfo.aliases.entries);
  const T = data.bindings.options.types;
  const execArgv = [];
  let script = null;
  let scriptArgs = [];
  let hasEval = false;
  const badOption = (message) => {
    hn.writeAll(2, hn.argv[0] + ": " + message + "\n");
    hn.exit(9);
  };
  const setOption = (name, value, queue) => {
    let negated = false;
    let option = info.get(name);
    if (!option && name.startsWith("--no-") && info.has("--" + name.slice(5))) {
      negated = true;
      name = "--" + name.slice(5);
      option = info.get(name);
    }
    if (!option) {
      // What is not node's is taken for the engine's, and the engine here is not ours to tell.
      return;
    }
    switch (option.type) {
    case T.kBoolean:
      values[name] = !negated;
      values["--no-" + name.slice(2)] = negated;
      if (name === "--print" && !negated) {
        hasEval = hasEval || values["--eval"] !== undefined;
      }
      break;
    case T.kInteger: case T.kUInteger: case T.kString: case T.kStringList: case T.kHostPort: {
      if (value === undefined) {
        if (!queue.length) {
          badOption(name + " requires an argument");
        }
        value = queue.shift();
      }
      if (option.type === T.kStringList) {
        values[name] = [...values[name], value];
      } else if (option.type === T.kString) {
        values[name] = value;
        if (name === "--eval") {
          hasEval = true;
        }
      } else if (option.type === T.kHostPort) {
        // (No inspector here.)
      } else {
        values[name] = Number(value);
      }
      break;
    }
    default:
      break;
    }
  };
  const parseOptions = (args, fromEnv) => {
    const queue = [...args];
    while (queue.length) {
      const arg = queue[0];
      if (arg === "--" && !fromEnv) {
        queue.shift();
        break;
      }
      if (arg[0] !== "-" || arg === "-") {
        break;
      }
      queue.shift();
      if (!fromEnv) {
        execArgv.push(arg);
      }
      let name = arg, value;
      const eq = arg.indexOf("=");
      if (arg.startsWith("--") && eq > 0) {
        name = arg.slice(0, eq);
        value = arg.slice(eq + 1);
      }
      // An alias stands for other arguments, which take its place (src/node_options-inl.h: Parse). One
      // written "--print <arg>" is meant when what follows is not an option: the script itself.
      const seen = new Set();
      for (;;) {
        let expansion = value !== undefined ? aliases.get(name + "=") : undefined;
        if (!expansion) {
          expansion = aliases.get(name);
        }
        if (!expansion && value === undefined && queue.length && queue[0][0] !== "-") {
          expansion = aliases.get(name + " <arg>");
        }
        if (!expansion || seen.has(name)) {
          break;
        }
        seen.add(name);
        queue.unshift(...expansion.slice(1));
        name = expansion[0];
      }
      setOption(name, value, queue);
      if (!fromEnv && values[name] !== undefined && info.get(name) && info.get(name).type >= T.kInteger &&
          value === undefined) {
        execArgv.push(String(Array.isArray(values[name]) ? values[name][values[name].length - 1] : values[name]));
      }
    }
    return queue;
  };
  const env = new Map();
  for (const entry of hn.envp) {
    const eq = entry.indexOf("=");
    if (eq > 0 && !env.has(entry.slice(0, eq))) {
      env.set(entry.slice(0, eq), entry.slice(eq + 1));
    }
  }
  hn.env = env;
  if (env.has("NODE_OPTIONS")) {
    parseOptions(env.get("NODE_OPTIONS").split(/\s+/).filter((s) => s !== ""), true);
  }
  const rest = parseOptions(hn.argv.slice(1), false);
  if (!hasEval && rest.length) {
    script = rest[0];
    scriptArgs = rest.slice(1);
  } else {
    scriptArgs = rest;
  }
  values["[has_eval_string]"] = hasEval;
  values["--no[has_eval_string]"] = !hasEval;
  hn.options = values;

  if (values["--version"]) {
    hn.writeAll(1, data.version + "\n");
    hn.exit(0);
  }

  // --- internalBinding(): each binding is written in one of the files of binding/, under its name.
  const bindingFiles = {
    core: ["async_wrap", "builtins", "config", "constants", "credentials", "errors", "mksnapshot", "options",
           "permission", "process_methods", "report", "symbols", "task_queue", "timers", "trace_events", "types",
           "util", "uv", "worker", "wasm_web_api", "inspector", "performance", "heap_utils", "v8", "os",
           "signal_wrap", "serdes", "watchdog", "internal_only_v8", "sea", "profiler", "block_list", "http2"],
    text: ["buffer", "string_decoder", "encoding_binding", "icu", "url", "blob", "messaging"],
    code: ["contextify", "module_wrap"],
    fs: ["fs", "fs_dir", "fs_event_wrap"],
    stream: ["stream_wrap", "tcp_wrap", "pipe_wrap", "tty_wrap", "js_stream", "stream_pipe", "udp_wrap",
             "cares_wrap"],
    process: ["process_wrap", "spawn_sync"],
    crypto: ["crypto"],
    http: ["http_parser"],
    zlib: ["zlib"],
    tls: ["tls_wrap"],
  };
  const bindings = { __proto__: null };
  const factories = { __proto__: null };
  const loadedFiles = new Set();
  // A file of bindings calls this for each: the function makes the binding when it is first asked for.
  hn.binding = (name, factory) => {
    factories[name] = factory;
  };
  const getInternalBinding = (name) => {
    if (name in bindings) {
      return bindings[name];
    }
    if (!(name in factories)) {
      for (const file of Object.keys(bindingFiles)) {
        if (bindingFiles[file].includes(name) && !loadedFiles.has(file)) {
          loadedFiles.add(file);
          hn.load("binding/" + file + ".js");
        }
      }
    }
    if (!(name in factories)) {
      if (!(name in data.bindings)) {
        throw new Error("No such binding: " + name);
      }
      // One that node has and nothing here is written for: its shape, so that the modules of lib/ that
      // stand on it can be loaded; what they then call says that it is not there.
      getInternalBinding("uv");
      factories[name] = () => hn.complete(name, {});
    }
    return (bindings[name] = factories[name](getInternalBinding));
  };
  hn.internalBinding = getInternalBinding;

  // --- Node's library: a file per module, compiled when first required (src/node_builtins.cc:
  // BuiltinLoader::LookupAndCompile, which has these lists of parameters).
  const parameters = (id) =>
    id === "internal/bootstrap/realm" ? "process, getLinkedBinding, getInternalBinding, primordials" :
    id.startsWith("internal/per_context/") ? "exports, primordials" :
    id.startsWith("internal/main/") || id.startsWith("internal/bootstrap/") ?
      "process, require, internalBinding, primordials" :
    "exports, require, module, process, internalBinding, primordials";
  // By module: a function from the text of the module to the text that is compiled instead. Node's library is
  // run as it is; these are the few places where that cannot be.
  hn.patches = {
    __proto__: null,
    // fetch() does not ask servers for Brotli, which is not here to undo it (binding/zlib.js).
    "internal/deps/undici/undici": (source) => source.split('"br, gzip, deflate"').join('"gzip, deflate"'),
    // Web Cryptography is the browser's own (binding/crypto.js), not node's implementation on OpenSSL.
    "internal/crypto/webcrypto": () => "module.exports = internalBinding('crypto').__webcrypto();",
  };
  hn.builtinSource = (id) => {
    let source = hn.readText(hn.home + "/lib/" + id + ".js");
    if (source === null) {
      throw new Error("hostnode: no such builtin module: " + id);
    }
    return source;
  };
  hn.compileBuiltin = (id) => {
    let source = hn.builtinSource(id);
    if (id in hn.patches) {
      source = hn.patches[id](source);
    }
    return (0, eval)("(function (" + parameters(id) + ") {" + source + "\n})\n//# sourceURL=node:" + id);
  };

  // --- primordials and the scripts that are run for every context (src/api/environment.cc:
  // InitializePrimordials).
  const primordials = { __proto__: null };
  const perContext = { primordials };
  for (const id of ["internal/per_context/primordials", "internal/per_context/domexception",
                    "internal/per_context/messageport"]) {
    hn.compileBuiltin(id)(perContext, primordials);
  }
  hn.primordials = primordials;
  hn.perContext = perContext;

  // --- The process object (src/node_process_object.cc: CreateProcessObject).
  const process = new (function process() {})();
  hn.process = process;
  const exitInfo = new Int32Array(3);	// kExiting, kExitCode, kHasExitCode
  hn.exitInfo = exitInfo;
  process[privateSymbols.exit_info_private_symbol] = exitInfo;
  const readonly = (object, name, value) =>
    Object.defineProperty(object, name, { value, writable: false, enumerable: true, configurable: true });
  readonly(process, "version", data.version);
  readonly(process, "versions", { ...data.versions });
  // What the program is told it runs on: the guest system, whose programs it starts and whose files it reads.
  readonly(process, "arch", data.versions.arch || data.config.variables.host_arch);
  readonly(process, "platform", "linux");
  readonly(process, "release", { ...data.release });
  process._rawDebug = (...args) => hn.writeAll(2, args.join(" ") + "\n");

  hn.script = script;
  hn.scriptArgs = scriptArgs;
  hn.execArgv = execArgv;

  // --- The realm, then the rest of the bootstrap (src/node_realm.cc: Realm::RunBootstrapping,
  // PrincipalRealm::BootstrapRealm).
  let internalBinding, requireBuiltin;
  hn.setInternalLoaders = (binding, require) => {
    internalBinding = binding;
    requireBuiltin = require;
    hn.requireBuiltin = require;
  };
  const getLinkedBinding = (name) => {
    throw new Error("No such binding: " + name);
  };
  hn.compileBuiltin("internal/bootstrap/realm")(process, getLinkedBinding, getInternalBinding, primordials);
  const bootstrap = (id) => hn.compileBuiltin(id)(process, requireBuiltin, internalBinding, primordials);
  bootstrap("internal/bootstrap/node");
  bootstrap("internal/bootstrap/web/exposed-wildcard");
  bootstrap("internal/bootstrap/web/exposed-window-or-worker");
  bootstrap("internal/bootstrap/switches/is_main_thread");
  bootstrap("internal/bootstrap/switches/does_own_process_state");

  // process.env: the environment as the program has changed it. Other programs get it when they are
  // started (binding/process.js), so nothing has to be told to the kernel.
  process.env = new Proxy({}, {
    get(target, key) {
      return typeof key === "string" ? env.get(key) : undefined;
    },
    set(target, key, value) {
      if (typeof key === "string") {
        env.set(key, String(value));
      }
      return true;
    },
    has(target, key) {
      return typeof key === "string" && env.has(key);
    },
    deleteProperty(target, key) {
      if (typeof key === "string") {
        env.delete(key);
      }
      return true;
    },
    ownKeys() {
      return [...env.keys()];
    },
    getOwnPropertyDescriptor(target, key) {
      return typeof key === "string" && env.has(key) ?
        { value: env.get(key), writable: true, enumerable: true, configurable: true } : undefined;
    },
    defineProperty(target, key, descriptor) {
      if (typeof key === "string" && "value" in descriptor) {
        env.set(key, String(descriptor.value));
        return true;
      }
      return false;
    },
  });

  // --- Callbacks into the program (src/api/callback.cc: InternalCallbackScope). After each: the ticks,
  // then, when we return to the browser, the jobs of promises.
  const tickInfo = getInternalBinding("task_queue").tickInfo;
  const errorsBinding = getInternalBinding("errors");
  hn.uncaught = (error) => errorsBinding.triggerUncaughtException(error, false);
  const ticks = () => {
    while (tickInfo[0] === 1 || tickInfo[1] === 1) {	// kHasTickScheduled, kHasRejectionToWarn
      try {
        hn.tickCallback();
        return;
      } catch (error) {
        hn.uncaught(error);
      }
    }
  };
  loop.ticksPending = () => tickInfo[0] === 1 || tickInfo[1] === 1;
  loop.runTicks = ticks;
  loop.call = (fn) => {
    try {
      fn();
    } catch (error) {
      if (error && error.name === "Trap") {
        throw error;
      }
      hn.uncaught(error);
    }
    ticks();
  };

  // --- The end of the program (src/api/hooks.cc: EmitProcessBeforeExit, EmitProcessExitInternal).
  const exitCode = () => (exitInfo[2] ? exitInfo[1] : 0);
  hn.emitExit = () => {
    exitInfo[0] = 1;
    const code = exitCode();
    try {
      process.emit("exit", code);
    } catch (error) {
      hn.uncaught(error);
    }
    return exitInfo[2] ? exitInfo[1] : code;
  };
  loop.finish = () => {
    loop.call(() => process.emit("beforeExit", exitCode()));
    if (loop.alive() || loop.ticksPending()) {
      return;
    }
    hn.exit(hn.emitExit());
  };

  // --- The main script (src/node.cc: StartExecution).
  let main;
  if (script === "inspect") {
    main = "internal/main/inspect";
  } else if (values["--help"]) {
    main = "internal/main/print_help";
  } else if (hasEval && !values["--interactive"]) {
    main = "internal/main/eval_string";
  } else if (values["--check"]) {
    main = "internal/main/check_syntax";
  } else if (values["--test"]) {
    main = "internal/main/test_runner";
  } else if (script !== null && script !== "-") {
    main = "internal/main/run_main_module";
  } else if (values["--interactive"] || getInternalBinding("tty_wrap").isTTY(0)) {
    main = "internal/main/repl";
  } else {
    main = "internal/main/eval_stdin";
  }
  loop.call(() => bootstrap(main));
  loop.start();
})
