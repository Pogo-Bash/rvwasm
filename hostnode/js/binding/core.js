// The small bindings: what Node's library asks of the process, the engine and
// the loop. The shape of each is that of the guest's own node (node.json has
// it: hostnode/extract.js); constants are that node's.
(function (hn) {
  "use strict";

  const { loop, data, SYS, sys0, sys1, sys2, sys3, sys4, dv, u8, S, web } = hn;
  const shapes = data.bindings;
  const EINTR = 4;

  // A function for everything a binding has that is not written here: the program is told what it was.
  const missing = (binding, name) => function () {
    const error = new Error("hostnode: " + binding + "." + name + " is not implemented");
    error.code = "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM";
    throw error;
  };
  // Fill a binding up to its shape in the guest's node: constants as they are, functions as `missing`.
  hn.complete = (name, binding) => {
    const shape = shapes[name] || {};
    for (const key of Object.keys(shape)) {
      if (key in binding) {
        continue;
      }
      const v = shape[key];
      if (v === null || typeof v !== "object") {
        binding[key] = v;
      } else if (v.$ === "function") {
        binding[key] = missing(name, key);
      } else if (typeof v.$ === "string" && typeof globalThis[v.$] === "function" && typeof v.length === "number" &&
                 /Array$/.test(v.$)) {
        binding[key] = new globalThis[v.$](v.length);	// numbers that lib/ and the binding share
      } else if (v.$ === undefined && !Array.isArray(v)) {
        binding[key] = JSON.parse(JSON.stringify(v));
      } else if (Array.isArray(v)) {
        binding[key] = JSON.parse(JSON.stringify(v));
      }
    }
    return binding;
  };
  hn.missing = missing;

  // --- uv: names and texts of errors. An error of the kernel is minus its number, as in libuv.
  const errors = new Map(data.uvErrors);
  hn.errname = (code) => (errors.has(code) ? errors.get(code)[0] : "Unknown system error " + code);
  hn.binding("uv", () => {
    const binding = {
      errname: hn.errname,
      getErrorMap: () => new Map(data.uvErrors),
    };
    for (const [code, [name]] of data.uvErrors) {
      binding["UV_" + name] = code;
    }
    return binding;
  });
  // What the library's errors carry (lib/internal/errors.js: UVException takes it from a `ctx`).
  hn.fail = (ctx, code, syscall, path, dest) => {
    ctx.errno = code;
    ctx.code = hn.errname(code);
    ctx.message = errors.has(code) ? errors.get(code)[1] : "unknown error";
    ctx.syscall = syscall;
    if (path !== undefined) {
      ctx.path = path;
    }
    if (dest !== undefined) {
      ctx.dest = dest;
    }
  };
  // The same as an exception, for the bindings that throw (src/api/exceptions.cc: UVException).
  hn.uvException = (code, syscall, path, dest, top) => {
    const name = hn.errname(code);
    let message = name + ": " + (errors.has(code) ? errors.get(code)[1] : "unknown error") + ", " + syscall;
    if (path !== undefined) {
      message += " '" + path + "'";
    }
    if (dest !== undefined) {
      message += " -> '" + dest + "'";
    }
    const error = new Error(message);
    // The trace starts where lib/ called the binding, as it does when the binding is C++.
    Error.captureStackTrace(error, top || hn.uvException);
    error.errno = code;
    error.code = name;
    error.syscall = syscall;
    if (path !== undefined) {
      error.path = path;
    }
    if (dest !== undefined) {
      error.dest = dest;
    }
    return error;
  };

  // --- builtins
  hn.binding("builtins", () => {
    const ids = data.ids;
    const cannot = new Set(ids.filter((id) => id.startsWith("internal/")));
    const can = new Set(ids.filter((id) => !id.startsWith("internal/")));
    const binding = {
      builtinIds: ids,
      builtinCategories: { cannotBeRequired: cannot, canBeRequired: can },
      config: shapes.builtins.config,
      compileFunction: hn.compileBuiltin,
      hasCachedBuiltins: () => true,
      getCacheUsage: () => ({ compiledWithCache: new Set(), compiledWithoutCache: new Set(), compiledInSnapshot: [] }),
      setInternalLoaders: hn.setInternalLoaders,
    };
    let natives = null;
    Object.defineProperty(binding, "natives", {
      enumerable: true,
      get() {
        if (natives === null) {
          natives = {};
          for (const id of ids) {
            natives[id] = hn.builtinSource(id);
          }
        }
        return natives;
      },
    });
    return binding;
  });

  hn.binding("config", () => ({ ...shapes.config, fipsMode: false }));
  hn.binding("constants", () => JSON.parse(JSON.stringify(shapes.constants)));

  hn.binding("symbols", () => {
    const binding = {};
    for (const name of Object.keys(shapes.symbols)) {
      binding[name] = Symbol(shapes.symbols[name].description);
    }
    return binding;
  });

  hn.binding("options", () => ({
    getCLIOptionsValues: () => hn.options,
    getCLIOptionsInfo: () => ({
      options: new Map(data.optionsInfo.options.entries),
      aliases: new Map(data.optionsInfo.aliases.entries),
    }),
    getEmbedderOptions: () => ({
      shouldNotRegisterESMLoader: false, noGlobalSearchPaths: false, noBrowserGlobals: false,
      hasEmbedderPreload: false,
    }),
    envSettings: shapes.options.envSettings,
    types: shapes.options.types,
  }));

  // --- errors
  const fatal = { before: null, after: null };
  hn.binding("errors", () => ({
    setPrepareStackTraceCallback(fn) {
      hn.prepareStackTraceCallback = fn;
    },
    setGetSourceMapErrorSource() {},
    setSourceMapsEnabled() {},
    setMaybeCacheGeneratedSourceMap() {},
    setEnhanceStackForFatalException(before, after) {
      fatal.before = before;
      fatal.after = after;
    },
    noSideEffectsToString: (value) => {
      try {
        return String(value);
      } catch (error) {
        return "[object]";
      }
    },
    // src/node_errors.cc: TriggerUncaughtException.
    triggerUncaughtException(error, fromPromise) {
      const process = hn.process;
      let handled;
      if (typeof process._fatalException !== "function") {
        report(error);
        hn.exit(6);
      }
      try {
        handled = process._fatalException(error, !!fromPromise);
      } catch (inner) {
        // The handler threw: lib/ has said what it had to say and set the exit code.
        report(inner);
        hn.exit(hn.exitInfo[2] ? hn.exitInfo[1] : 7);
      }
      if (handled === false) {
        report(error);
        hn.exit(hn.exitInfo[2] && hn.exitInfo[1] ? hn.exitInfo[1] : 1);
      }
    },
    exitCodes: shapes.errors.exitCodes,
  }));
  // src/node_errors.cc: ReportFatalException, without the line of source (the engine does not hand it out).
  const report = (error) => {
    let text;
    try {
      if (error !== null && typeof error === "object" && typeof error.stack === "string") {
        text = fatal.after ? fatal.after(error) : error.stack;
      } else {
        text = "Uncaught:\n" + hn.requireBuiltin("internal/util/inspect").inspect(error);
      }
    } catch (inner) {
      try {
        text = String(error && error.stack || error);
      } catch (innermost) {
        text = "Uncaught exception";
      }
    }
    hn.writeAll(2, text + "\n\nNode.js " + data.version + "\n");
  };

  // --- task_queue
  hn.binding("task_queue", () => {
    const tickInfo = new Uint8Array(2);
    let rejections = null;
    const listen = () => {
      // The browser tells of promises rejected with nobody listening, as an event, once the jobs of
      // promises have run: which is when Node would look.
      if (!web.addEventListener) {
        return;
      }
      web.addEventListener("unhandledrejection", (event) => {
        event.preventDefault();
        rejections(0, event.promise, event.reason);	// kPromiseRejectWithNoHandler
      });
      web.addEventListener("rejectionhandled", (event) => {
        rejections(1, event.promise, undefined);	// kPromiseHandlerAddedAfterReject
      });
    };
    return {
      tickInfo,
      // The engine runs them itself whenever our code has returned (see loop.js).
      runMicrotasks() {},
      setTickCallback(fn) {
        hn.tickCallback = fn;
      },
      enqueueMicrotask(fn) {
        web.queueMicrotask(() => {
          try {
            fn();
          } catch (error) {
            hn.uncaught(error);
          }
        });
      },
      promiseRejectEvents: shapes.task_queue.promiseRejectEvents,
      setPromiseRejectCallback(fn) {
        if (rejections === null) {
          rejections = fn;
          listen();
        }
        rejections = fn;
      },
    };
  });

  // --- timers (src/timers.cc)
  hn.binding("timers", () => {
    const immediateInfo = new Uint32Array(3);	// kCount, kRefCount, kHasOutstanding
    const timeoutInfo = new Int32Array(1);
    loop.immediateInfo = immediateInfo;
    const now = () => Math.floor(loop.now());
    return {
      immediateInfo,
      timeoutInfo,
      setupTimers(processImmediate, processTimers) {
        loop.immediateCallback = () => {
          do {
            try {
              processImmediate();
            } catch (error) {
              hn.uncaught(error);
            }
          } while (immediateInfo[2]);
        };
        loop.timerCallback = () => {
          let expiry;
          for (;;) {
            try {
              expiry = processTimers(now());
              break;
            } catch (error) {
              hn.uncaught(error);
            }
          }
          if (expiry !== 0) {
            loop.timerDue = loop.now() + Math.max(1, Math.abs(expiry) - now());
            loop.timerRef = expiry > 0;
          }
        };
      },
      getLibuvNow: now,
      scheduleTimer(duration) {
        loop.timerDue = loop.now() + Number(duration);
      },
      toggleTimerRef(ref) {
        loop.timerRef = !!ref;
      },
      toggleImmediateRef() {},
    };
  });

  // --- async_wrap: the numbers behind async_hooks (src/async_wrap.cc, src/env.cc: AsyncHooks).
  hn.binding("async_wrap", () => {
    const c = shapes.async_wrap.constants;
    const fields = new Uint32Array(9);
    const ids = new Float64Array(4);
    ids[c.kAsyncIdCounter] = 1;
    ids[c.kDefaultTriggerAsyncId] = -1;
    const resources = [];
    const binding = {
      async_hook_fields: fields,
      async_id_fields: ids,
      async_ids_stack: new Float64Array(32),
      execution_async_resources: resources,
      constants: c,
      Providers: shapes.async_wrap.Providers,
      setupHooks(hooks) {
        hn.asyncHooks = hooks;
      },
      setCallbackTrampoline() {},
      pushAsyncContext(asyncId, triggerAsyncId, resource) {
        const offset = fields[c.kStackLength];
        if (offset * 2 >= binding.async_ids_stack.length) {
          const bigger = new Float64Array(binding.async_ids_stack.length * 3);
          bigger.set(binding.async_ids_stack);
          binding.async_ids_stack = bigger;
        }
        binding.async_ids_stack[offset * 2] = ids[c.kExecutionAsyncId];
        binding.async_ids_stack[offset * 2 + 1] = ids[c.kTriggerAsyncId];
        fields[c.kStackLength]++;
        ids[c.kExecutionAsyncId] = asyncId;
        ids[c.kTriggerAsyncId] = triggerAsyncId;
        resources[offset] = resource;
      },
      popAsyncContext(asyncId) {
        const offset = fields[c.kStackLength] - 1;
        if (offset < 0) {
          return false;
        }
        ids[c.kExecutionAsyncId] = binding.async_ids_stack[2 * offset];
        ids[c.kTriggerAsyncId] = binding.async_ids_stack[2 * offset + 1];
        resources.length = offset;
        fields[c.kStackLength] = offset;
        return offset > 0;
      },
      executionAsyncResource: (index) => resources[index],
      clearAsyncIdStack() {
        ids[c.kExecutionAsyncId] = 0;
        ids[c.kTriggerAsyncId] = 0;
        fields[c.kStackLength] = 0;
        resources.length = 0;
      },
      queueDestroyAsyncId(id) {
        if (fields[c.kDestroy] > 0 && hn.asyncHooks) {
          loop.enqueue(() => hn.asyncHooks.destroy(id));
        }
      },
      // The engine has no hooks into promises for us: what async_hooks tells of promises, it does not here.
      setPromiseHooks() {},
      getPromiseHooks: () => [undefined, undefined, undefined, undefined],
      registerDestroyHook() {},
    };
    hn.async = { binding, fields, ids, c };
    return binding;
  });

  // What every handle and request of node's C++ is (src/async_wrap.h: AsyncWrap): a thing with a number for
  // async_hooks, whose callbacks into the program are announced to the hooks.
  class AsyncWrap {
    constructor(provider) {
      const a = hn.async || (hn.internalBinding("async_wrap"), hn.async);
      this.__provider = provider;
      this.__asyncId = ++a.ids[a.c.kAsyncIdCounter];
      const trigger = a.ids[a.c.kDefaultTriggerAsyncId];
      this.__triggerId = trigger >= 0 ? trigger : a.ids[a.c.kExecutionAsyncId];
      if (a.fields[a.c.kInit] > 0 && hn.asyncHooks) {
        hn.asyncHooks.init(this.__asyncId, provider, this.__triggerId, this);
      }
    }
    getAsyncId() {
      return this.__asyncId;
    }
    getProviderType() {
      return shapes.async_wrap.Providers[this.__provider] || 0;
    }
    asyncReset(resource) {
      const a = hn.async;
      this.__asyncId = ++a.ids[a.c.kAsyncIdCounter];
      const trigger = a.ids[a.c.kDefaultTriggerAsyncId];
      this.__triggerId = trigger >= 0 ? trigger : a.ids[a.c.kExecutionAsyncId];
      if (a.fields[a.c.kInit] > 0 && hn.asyncHooks) {
        hn.asyncHooks.init(this.__asyncId, this.__provider, this.__triggerId, resource || this);
      }
    }
  }
  for (const name of ["__provider", "__asyncId", "__triggerId"]) {
    Object.defineProperty(AsyncWrap.prototype, name, { writable: true, enumerable: false, value: 0 });
  }
  hn.AsyncWrap = AsyncWrap;
  // Call a method of a handle from the loop (src/api/callback.cc: InternalMakeCallback).
  hn.makeCallback = (wrap, fn) => {
    const a = hn.async;
    const hooks = hn.asyncHooks;
    if (a.fields[a.c.kBefore] > 0) {
      hooks.before(wrap.__asyncId);
    }
    a.binding.pushAsyncContext(wrap.__asyncId, wrap.__triggerId, wrap);
    try {
      return fn();
    } finally {
      a.binding.popAsyncContext(wrap.__asyncId);
      if (a.fields[a.c.kAfter] > 0) {
        hooks.after(wrap.__asyncId);
      }
    }
  };
  // The same, later, on a turn of its own.
  hn.callbackLater = (wrap, fn) => loop.enqueue(() => hn.makeCallback(wrap, fn));
  // A request's oncomplete, if it still has one: lib/ takes it away from a request it has given up on (net.js,
  // a connection attempt that took too long), and node's C++ then calls nothing.
  hn.oncomplete = (request, ...args) =>
    (typeof request.oncomplete === "function" ? request.oncomplete(...args) : undefined);

  // A handle that keeps the loop going while it is open, referenced and doing something (src/handle_wrap.cc,
  // and libuv's notion of an active handle).
  class HandleWrap extends AsyncWrap {
    constructor(provider) {
      super(provider);
      this.__refed = true;
      this.__closed = false;
      this.__active = false;
      this.__counted = false;	// whether the loop is counting this one right now
      this.__leader = null;	// another handle whose ref() and unref() are this one's too (TLS over a stream)
      this.__followers = null;
    }
    __recount() {
      const refed = this.__leader ? this.__leader.__refed && !this.__leader.__closed : this.__refed;
      const should = this.__active && refed && !this.__closed;
      if (should !== this.__counted) {
        this.__counted = should;
        loop.refs += should ? 1 : -1;
      }
      if (this.__followers) {
        for (const follower of this.__followers) {
          follower.__recount();
        }
      }
    }
    __follow(leader) {
      this.__leader = leader;
      (leader.__followers || (leader.__followers = [])).push(this);
      this.__recount();
    }
    __setActive(active) {
      if (active !== this.__active) {
        this.__active = active;
        this.__recount();
      }
    }
    ref() {
      if (!this.__refed && !this.__closed) {
        this.__refed = true;
        this.__recount();
      }
    }
    unref() {
      if (this.__refed && !this.__closed) {
        this.__refed = false;
        this.__recount();
      }
    }
    hasRef() {
      return this.__refed && !this.__closed;
    }
    close(callback) {
      if (this.__closed) {
        return;
      }
      this.__active = false;
      this.__closed = true;
      this.__recount();
      if (this.__close) {
        this.__close();
      }
      // The closing itself keeps the loop for one more turn.
      loop.refs++;
      loop.enqueue(() => {
        loop.refs--;
        hn.makeCallback(this, () => {
          const onclose = this[hn.internalBinding("symbols").handle_onclose];
          if (typeof callback === "function") {
            callback.call(this);
          } else if (typeof onclose === "function") {
            onclose.call(this);
          }
        });
      });
    }
  }
  for (const name of ["__refed", "__closed", "__active", "__counted", "__leader", "__followers"]) {
    Object.defineProperty(HandleWrap.prototype, name, { writable: true, enumerable: false, value: false });
  }
  hn.HandleWrap = HandleWrap;

  // --- types: what kind of object something is. The engine knows; from JavaScript it shows in how the
  // object names itself, which an object can lie about, so a yes is checked with a method that only works
  // on the real thing.
  const toString = Object.prototype.toString;
  const tag = (v) => toString.call(v);
  const real = (method, v) => {
    try {
      method.call(v);
      return true;
    } catch (error) {
      return false;
    }
  };
  const isObject = (v) => v !== null && typeof v === "object";
  const mapSize = Object.getOwnPropertyDescriptor(Map.prototype, "size").get;
  const setSize = Object.getOwnPropertyDescriptor(Set.prototype, "size").get;
  const abLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get;
  const sabLength = Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "byteLength").get;
  const dvLength = Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get;
  const regexpSource = Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get;
  const typedTag = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
  const types = {
    isExternal: () => false,
    isDate: (v) => isObject(v) && tag(v) === "[object Date]" && real(Date.prototype.getTime, v),
    isArgumentsObject: (v) => isObject(v) && tag(v) === "[object Arguments]",
    isBigIntObject: (v) => isObject(v) && tag(v) === "[object BigInt]" && real(BigInt.prototype.valueOf, v),
    isBooleanObject: (v) => isObject(v) && tag(v) === "[object Boolean]" && real(Boolean.prototype.valueOf, v),
    isNumberObject: (v) => isObject(v) && tag(v) === "[object Number]" && real(Number.prototype.valueOf, v),
    isStringObject: (v) => isObject(v) && tag(v) === "[object String]" && real(String.prototype.valueOf, v),
    isSymbolObject: (v) => isObject(v) && tag(v) === "[object Symbol]" && real(Symbol.prototype.valueOf, v),
    isNativeError: typeof Error.isError === "function" ? (v) => Error.isError(v) :
      (v) => isObject(v) && (tag(v) === "[object Error]" || (v instanceof Error && !(Symbol.toStringTag in v))),
    isRegExp: (v) => isObject(v) && tag(v) === "[object RegExp]" && real(regexpSource, v),
    isAsyncFunction: (v) => typeof v === "function" &&
      (tag(v) === "[object AsyncFunction]" || tag(v) === "[object AsyncGeneratorFunction]"),
    isGeneratorFunction: (v) => typeof v === "function" &&
      (tag(v) === "[object GeneratorFunction]" || tag(v) === "[object AsyncGeneratorFunction]"),
    isGeneratorObject: (v) => isObject(v) && tag(v) === "[object Generator]",
    isPromise: (v) => isObject(v) && tag(v) === "[object Promise]" && v instanceof Promise,
    isMap: (v) => isObject(v) && tag(v) === "[object Map]" && real(mapSize, v),
    isSet: (v) => isObject(v) && tag(v) === "[object Set]" && real(setSize, v),
    isMapIterator: (v) => isObject(v) && tag(v) === "[object Map Iterator]",
    isSetIterator: (v) => isObject(v) && tag(v) === "[object Set Iterator]",
    isWeakMap: (v) => isObject(v) && tag(v) === "[object WeakMap]" && real(function () { WeakMap.prototype.has.call(this, {}); }, v),
    isWeakSet: (v) => isObject(v) && tag(v) === "[object WeakSet]" && real(function () { WeakSet.prototype.has.call(this, {}); }, v),
    isArrayBuffer: (v) => isObject(v) && tag(v) === "[object ArrayBuffer]" && real(abLength, v),
    isDataView: (v) => isObject(v) && tag(v) === "[object DataView]" && real(dvLength, v),
    isSharedArrayBuffer: (v) => isObject(v) && tag(v) === "[object SharedArrayBuffer]" && real(sabLength, v),
    // A proxy cannot be told from what it stands for, from here.
    isProxy: () => false,
    isModuleNamespaceObject: (v) => isObject(v) && tag(v) === "[object Module]" && Object.getPrototypeOf(v) === null,
    isAnyArrayBuffer: (v) => types.isArrayBuffer(v) || types.isSharedArrayBuffer(v),
    isBoxedPrimitive: (v) => types.isNumberObject(v) || types.isStringObject(v) || types.isBooleanObject(v) ||
      types.isBigIntObject(v) || types.isSymbolObject(v),
  };
  hn.types = types;
  hn.binding("types", () => ({ ...types }));

  // --- util
  hn.binding("util", () => {
    const c = shapes.util.constants;
    const O_PATH = 0;
    const binding = {
      privateSymbols: hn.privateSymbols,
      constants: c,
      shouldAbortOnUncaughtToggle: new Uint32Array(1),
      isInsideNodeModules: () => {
        const stack = String(new Error().stack).split("\n").slice(2);
        for (const line of stack) {
          if (line.includes("node:") || line.includes("hostnode:")) {
            continue;
          }
          return line.includes("/node_modules/");
        }
        return false;
      },
      // The state of a promise is not to be had without waiting for it.
      getPromiseDetails: () => [c.kPending],
      getProxyDetails: () => undefined,
      getCallerLocation: () => undefined,
      isArrayBufferDetached: (buffer) => {
        if (typeof buffer.detached === "boolean") {
          return buffer.detached;
        }
        if (buffer.byteLength !== 0) {
          return false;
        }
        try {
          new Uint8Array(buffer);
          return false;
        } catch (error) {
          return true;
        }
      },
      // What is in an iterator of a Map or Set, without taking from it: not to be had either.
      previewEntries: (value, isIterator) => (isIterator ? [[], false] : []),
      getOwnNonIndexProperties(object, filter) {
        const keys = [];
        for (const key of Reflect.ownKeys(object)) {
          if (typeof key === "string") {
            const n = Number(key);
            if (key !== "" && String(n >>> 0) === key && (n >>> 0) !== 0xffffffff) {
              continue;
            }
          }
          if (filter === c.ONLY_ENUMERABLE) {
            const d = Reflect.getOwnPropertyDescriptor(object, key);
            if (!d || !d.enumerable) {
              continue;
            }
          }
          keys.push(key);
        }
        return keys;
      },
      getConstructorName(object) {
        for (let o = object; o !== null && o !== undefined; o = Object.getPrototypeOf(o)) {
          const d = Object.getOwnPropertyDescriptor(o, "constructor");
          if (d && typeof d.value === "function" && d.value.name !== "") {
            return d.value.name;
          }
        }
        return "";
      },
      getExternalValue: () => 0n,
      sleep(ms) {
        const v = dv();
        v.setBigInt64(S, BigInt(Math.floor(ms / 1000)), true);
        v.setBigInt64(S + 8, BigInt(Math.floor(ms % 1000) * 1000000), true);
        while (sys2(SYS.nanosleep, S, S) === -EINTR) {
        }
      },
      parseEnv(content) {
        const result = {};
        for (let line of String(content).split(/\r?\n/)) {
          line = line.trim();
          if (line === "" || line[0] === "#") {
            continue;
          }
          if (line.startsWith("export ")) {
            line = line.slice(7).trim();
          }
          const eq = line.indexOf("=");
          if (eq <= 0) {
            continue;
          }
          const key = line.slice(0, eq).trim();
          let value = line.slice(eq + 1).trim();
          const quote = value[0];
          if ((quote === '"' || quote === "'" || quote === "`") && value.indexOf(quote, 1) > 0) {
            value = value.slice(1, value.indexOf(quote, 1));
            if (quote === '"') {
              value = value.replace(/\\n/g, "\n");
            }
          } else {
            const hash = value.indexOf(" #");
            if (hash >= 0) {
              value = value.slice(0, hash).trim();
            }
          }
          result[key] = value;
        }
        return result;
      },
      arrayBufferViewHasBuffer: () => true,
      // 0 TCP, 1 TTY, 2 UDP, 3 FILE, 4 PIPE, 5 UNKNOWN (lib/internal/util.js: handleTypes).
      guessHandleType(fd) {
        return hn.handleType(fd);
      },
      isArrayBufferView: (v) => ArrayBuffer.isView(v),
      isTypedArray: (v) => isObject(v) && typedTag.call(v) !== undefined,
      isUint8Array: (v) => isObject(v) && typedTag.call(v) === "Uint8Array",
    };
    for (const name of Object.keys(shapes.util)) {
      if (!(name in binding) && name in types) {
        binding[name] = types[name];
      }
    }
    void O_PATH;
    return binding;
  });
  // What a descriptor is, as libuv guesses it (uv_guess_handle).
  hn.handleType = (fd) => {
    const r = sys5statx(fd);
    if (r < 0) {
      return 5;
    }
    const mode = dv().getUint16(S + 28, true) & 0o170000;
    if (mode === 0o020000) {
      // A character device: a terminal if it answers as one.
      return sys3(SYS.ioctl, fd, 0x5401, S + 512) === 0 ? 1 : 3;
    }
    if (mode === 0o100000 || mode === 0o060000 || mode === 0o040000) {
      return 3;
    }
    if (mode === 0o010000) {
      return 4;
    }
    if (mode === 0o140000) {
      const v = dv();
      v.setUint32(S + 300, 4, true);
      if (sys5sockopt(fd, 1, 3, S + 304, S + 300) < 0) {	// SOL_SOCKET, SO_TYPE
        return 5;
      }
      const type = v.getInt32(S + 304, true);
      v.setUint32(S + 300, 128, true);
      if (hn.sys3(SYS.getsockname, fd, S + 320, S + 300) < 0) {
        return 5;
      }
      const family = v.getUint16(S + 320, true);
      if (type === 2) {
        return family === 2 || family === 10 ? 2 : 5;
      }
      if (type === 1) {
        return family === 2 || family === 10 ? 0 : family === 1 ? 4 : 5;
      }
    }
    return 5;
  };
  const sys5statx = (fd) => hn.sys5(SYS.statx, fd, hn.path("", hn.P0), 0x1000, 0x7ff, S);	// AT_EMPTY_PATH
  const sys5sockopt = (fd, level, name, value, length) => hn.sys5(SYS.getsockopt, fd, level, name, value, length);

  // --- credentials
  hn.binding("credentials", () => {
    const set = (number, name) => (id) => {
      if (typeof id !== "number") {
        throw new Error("hostnode: " + name + " by name is not implemented");
      }
      const r = sys1(number, id);
      if (r < 0) {
        throw hn.uvException(r, name);
      }
    };
    return hn.complete("credentials", {
      safeGetenv: (name) => hn.env.get(String(name)),
      implementsPosixCredentials: true,
      getuid: () => sys0(SYS.getuid),
      geteuid: () => sys0(SYS.geteuid),
      getgid: () => sys0(SYS.getgid),
      getegid: () => sys0(SYS.getegid),
      getgroups() {
        const n = sys2(SYS.getgroups, 1024, S);
        const groups = [];
        for (let i = 0; i < n; i++) {
          groups.push(dv().getUint32(S + 4 * i, true));
        }
        const egid = sys0(SYS.getegid);
        if (!groups.includes(egid)) {
          groups.push(egid);
        }
        return groups;
      },
      setuid: set(SYS.setuid, "setuid"),
      setgid: set(SYS.setgid, "setgid"),
      seteuid: (id) => {
        const r = hn.sys2(SYS.setreuid, -1, id);
        if (r < 0) {
          throw hn.uvException(r, "seteuid");
        }
      },
      setegid: (id) => {
        const r = hn.sys2(SYS.setregid, -1, id);
        if (r < 0) {
          throw hn.uvException(r, "setegid");
        }
      },
    });
  });

  // --- process_methods (src/node_process_methods.cc)
  hn.binding("process_methods", () => {
    const hrtimeBuffer = new Uint32Array(3);
    const hrtimeBig = new BigUint64Array(hrtimeBuffer.buffer, 0, 1);
    const started = web.performance.now();
    const ns = () => {
      const ms = web.performance.now();
      const whole = Math.floor(ms);
      return BigInt(whole) * 1000000n + BigInt(Math.round((ms - whole) * 1e6));
    };
    const title = { value: "node" };
    const binding = {
      hrtimeBuffer,
      // Seconds in two halves, then nanoseconds (lib/internal/process/per_thread.js reads them so).
      hrtime() {
        const t = ns();
        const seconds = t / 1000000000n;
        hrtimeBuffer[0] = Number(seconds >> 32n);
        hrtimeBuffer[1] = Number(seconds & 0xffffffffn);
        hrtimeBuffer[2] = Number(t % 1000000000n);
      },
      hrtimeBigInt() {
        hrtimeBig[0] = ns();
      },
      uptime: () => (web.performance.now() - started) / 1000,
      cwd() {
        const r = sys2(SYS.getcwd, S, hn.SCRATCH);
        if (r < 0) {
          throw hn.uvException(r, "uv_cwd");
        }
        return hn.cstring(S);
      },
      chdir(directory) {
        const r = sys1(SYS.chdir, hn.path(String(directory), hn.P0));
        if (r < 0) {
          throw hn.uvException(r, "chdir", String(directory));
        }
      },
      umask(mask) {
        if (mask === undefined) {
          const old = sys1(SYS.umask, 0);
          sys1(SYS.umask, old);
          return old;
        }
        return sys1(SYS.umask, mask);
      },
      _kill: (pid, signal) => sys2(SYS.kill, pid, signal),
      _rawDebug: (text) => {
        hn.writeAll(2, String(text) + "\n");
      },
      reallyExit: (code) => hn.exit(code === undefined ? 0 : code),
      abort() {
        sys2(SYS.kill, sys0(SYS.getpid), 6);
        hn.exit(134);
      },
      causeSegfault() {
        sys2(SYS.kill, sys0(SYS.getpid), 11);
        hn.exit(139);
      },
      // src/node_process_object.cc: PatchProcessObject.
      patchProcessObject(process) {
        Object.defineProperty(process, "title", {
          enumerable: true, configurable: true,
          get: () => title.value,
          set: (value) => {
            title.value = String(value);
          },
        });
        process.argv = [hn.argv[0], ...(hn.script !== null ? [hn.script] : []), ...hn.scriptArgs];
        process.execArgv = hn.execArgv;
        const fixed = (name, value) =>
          Object.defineProperty(process, name, { value, writable: false, enumerable: true, configurable: true });
        fixed("pid", sys0(SYS.getpid));
        Object.defineProperty(process, "ppid", {
          enumerable: true, configurable: true, get: () => sys0(SYS.getppid),
        });
        // The program that a script would name to run another of its kind: us.
        let execPath = hn.argv[0];
        const r = hn.sys4(SYS.readlinkat, -100, hn.path("/proc/self/exe", hn.P0), S, hn.SCRATCH);
        if (r > 0) {
          execPath = hn.text(S, r);
        }
        process.execPath = execPath;
        Object.defineProperty(process, "debugPort", {
          enumerable: true, configurable: true, get: () => 9229, set() {},
        });
        fixed("versions", { ...data.versions });
      },
      setEmitWarningSync() {},
      memoryUsage(array) {
        // [rss, heapTotal, heapUsed, external, arrayBuffers]: what the engine says of itself, where it does.
        const m = web.performance.memory;
        array[0] = binding.rss();
        array[1] = m ? m.totalJSHeapSize : 0;
        array[2] = m ? m.usedJSHeapSize : 0;
        array[3] = 0;
        array[4] = 0;
      },
      rss: () => {
        const m = web.performance.memory;
        return m ? m.totalJSHeapSize : 0;
      },
      constrainedMemory: () => 0,
      availableMemory: () => {
        const r = sys1(SYS.sysinfo, S);
        return r < 0 ? 0 : Number(dv().getBigUint64(S + 40, true)) * dv().getUint32(S + 104, true);
      },
      cpuUsage(array) {
        // getrusage: user and system time in microseconds.
        if (sys2(SYS.getrusage, 0, S) < 0) {
          array[0] = array[1] = 0;
          return;
        }
        const v = dv();
        array[0] = Number(v.getBigInt64(S, true)) * 1e6 + Number(v.getBigInt64(S + 8, true));
        array[1] = Number(v.getBigInt64(S + 16, true)) * 1e6 + Number(v.getBigInt64(S + 24, true));
      },
      resourceUsage(array) {
        array.fill(0);
        if (sys2(SYS.getrusage, 0, S) < 0) {
          return;
        }
        const v = dv();
        array[0] = Number(v.getBigInt64(S, true)) * 1e6 + Number(v.getBigInt64(S + 8, true));
        array[1] = Number(v.getBigInt64(S + 16, true)) * 1e6 + Number(v.getBigInt64(S + 24, true));
        for (let i = 0; i < 14; i++) {
          array[2 + i] = Number(v.getBigInt64(S + 32 + 8 * i, true));
        }
      },
      _getActiveRequests: () => [],
      _getActiveHandles: () => [],
      getActiveResourcesInfo: () => [],
      _debugProcess() {},
      _debugEnd() {},
      dlopen(module, filename) {
        const error = new Error("hostnode: " + filename + " is machine code for a processor (" +
          hn.process.arch + ") that this Node.js does not run on: it is run by the browser's JavaScript engine. " +
          "Native addons cannot be loaded.");
        error.code = "ERR_DLOPEN_FAILED";
        throw error;
      },
      loadEnvFile(path) {
        const content = hn.readText(path === undefined ? ".env" : String(path));
        if (content === null) {
          throw hn.uvException(-2, "open", path === undefined ? ".env" : String(path));
        }
        const parsed = hn.internalBinding("util").parseEnv(content);
        for (const key of Object.keys(parsed)) {
          if (!hn.env.has(key)) {
            hn.env.set(key, parsed[key]);
          }
        }
      },
      resetStdioForTesting() {},
    };
    return binding;
  });

  // --- Things there is nothing of here, in the shapes lib/ expects.
  hn.binding("trace_events", () => ({
    getEnabledCategories: () => undefined,
    setTraceCategoryStateUpdateHandler() {},
    getCategoryEnabledBuffer: () => new Uint8Array(1),
    CategorySet: class CategorySet {
      enable() {}
      disable() {}
    },
    isTraceCategoryEnabled: () => false,
    trace() {},
  }));
  hn.binding("mksnapshot", () => hn.complete("mksnapshot", {
    setSerializeCallback() {},
    setDeserializeCallback() {},
    setDeserializeMainFunction() {},
    isBuildingSnapshotBuffer: new Uint8Array(1),
  }));
  hn.binding("permission", () => ({ has: () => true }));
  hn.binding("report", () => {
    const state = { compact: false, excludeNetwork: false, directory: "", filename: "", signal: "SIGUSR2",
                    fatal: false, onSignal: false, uncaught: false };
    return hn.complete("report", {
      getCompact: () => state.compact, setCompact: (v) => { state.compact = v; },
      getExcludeNetwork: () => state.excludeNetwork, setExcludeNetwork: (v) => { state.excludeNetwork = v; },
      getDirectory: () => state.directory, setDirectory: (v) => { state.directory = v; },
      getFilename: () => state.filename, setFilename: (v) => { state.filename = v; },
      getSignal: () => state.signal, setSignal: (v) => { state.signal = v; },
      shouldReportOnFatalError: () => state.fatal, setReportOnFatalError: (v) => { state.fatal = v; },
      shouldReportOnSignal: () => state.onSignal, setReportOnSignal: (v) => { state.onSignal = v; },
      shouldReportOnUncaughtException: () => state.uncaught,
      setReportOnUncaughtException: (v) => { state.uncaught = v; },
    });
  });
  hn.binding("worker", () => hn.complete("worker", {
    isMainThread: true,
    ownsProcessState: true,
    threadId: 0,
    resourceLimits: new Float64Array(4),
    getEnvMessagePort: () => undefined,
    Worker: class Worker {
      constructor() {
        const error = new Error("hostnode: worker threads are not implemented");
        error.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
        throw error;
      }
    },
  }));
  hn.binding("wasm_web_api", () => ({ setImplementation() {} }));
  hn.binding("inspector", () => {
    const binding = hn.complete("inspector", {
      isEnabled: () => false,
      setConsoleExtensionInstaller() {},
      registerAsyncHook() {},
      asyncTaskScheduled() {}, asyncTaskCanceled() {}, asyncTaskStarted() {}, asyncTaskFinished() {},
      emitProtocolEvent() {},
      setupNetworkTracking() {},
      url: () => undefined,
      // With an inspector, what the console prints goes to it as well: there is none.
      consoleCall(inspectorMethod, nodeMethod, ...args) {
        return Reflect.apply(nodeMethod, this, args);
      },
      console: {},
    });
    return binding;
  });
  hn.binding("performance", () => {
    const milestones = new Float64Array(8);
    const c = shapes.performance.constants;
    // In nanoseconds, since what performance.now() counts from.
    const origin = web.performance.timeOrigin;
    milestones[c.NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP] = origin * 1000;
    milestones[c.NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN] = 0;
    for (const name of ["ENVIRONMENT", "NODE_START", "V8_START", "LOOP_START", "BOOTSTRAP_COMPLETE"]) {
      if (("NODE_PERFORMANCE_MILESTONE_" + name) in c) {
        milestones[c["NODE_PERFORMANCE_MILESTONE_" + name]] = web.performance.now() * 1e6;
      }
    }
    return hn.complete("performance", {
      observerCounts: new Uint32Array(5),
      milestones,
      now: () => web.performance.now(),
      setupObservers() {},
      installGarbageCollectionTracking() {},
      removeGarbageCollectionTracking() {},
      notify() {},
      loopIdleTime: () => 0,
      markBootstrapComplete() {
        milestones[c.NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE] = web.performance.now() * 1e6;
      },
      uvMetricsInfo: () => [0, 0, 0],
    });
  });
  hn.binding("heap_utils", () => hn.complete("heap_utils", {}));
  hn.binding("v8", () => hn.complete("v8", {
    heapStatisticsBuffer: new Float64Array(14),
    heapCodeStatisticsBuffer: new Float64Array(4),
    heapSpaceStatisticsBuffer: new Float64Array(4),
    cachedDataVersionTag: () => 0,
    setHeapSnapshotNearHeapLimit() {},
    setFlagsFromString() {},
    updateHeapStatisticsBuffer() {
      const m = web.performance.memory;
      const b = this.heapStatisticsBuffer;
      if (m) {
        b[0] = m.totalJSHeapSize;
        b[2] = m.totalJSHeapSize;
        b[4] = m.usedJSHeapSize;
        b[5] = m.jsHeapSizeLimit;
        b[3] = m.jsHeapSizeLimit - m.usedJSHeapSize;
      }
    },
    updateHeapCodeStatisticsBuffer() {},
    updateHeapSpaceStatisticsBuffer() {},
  }));
  hn.binding("serdes", () => hn.complete("serdes", {}));
  hn.binding("watchdog", () => hn.complete("watchdog", {}));
  hn.binding("internal_only_v8", () => ({ queryObjects: () => [] }));
  hn.binding("sea", () => ({
    isSea: () => false, isExperimentalSeaWarningNeeded: () => false, getAsset: () => undefined,
  }));
  hn.binding("profiler", () => ({
    setCoverageDirectory() {}, setSourceMapCacheGetter() {}, takeCoverage() {}, endCoverage() {},
  }));
  // Lists of addresses (src/node_sockaddr.cc: SocketAddress, SocketAddressBlockList).
  hn.binding("block_list", () => {
    const AF_INET = 2, AF_INET6 = 10;
    hn.internalBinding("stream_wrap");	// (the address parsers are with the sockets)
    const bytesOf = (address, family) => {
      const b = family === AF_INET ? hn.net.parse4(address) : hn.net.parse6(address);
      if (!b) {
        const error = new Error("Invalid address");
        error.code = "ERR_INVALID_ADDRESS";
        throw error;
      }
      return b;
    };
    // As 16 bytes either way: an IPv4 address is also the IPv6 address that stands for it.
    const wide = (handle) => (handle.family === AF_INET ? [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, ...handle.bytes] : handle.bytes);
    const compare = (a, b) => {
      for (let i = 0; i < 16; i++) {
        if (a[i] !== b[i]) {
          return a[i] < b[i] ? -1 : 1;
        }
      }
      return 0;
    };
    class SocketAddress {
      constructor(address, port, family, flowlabel) {
        this.address = address;
        this.port = port;
        this.family = family;
        this.label = flowlabel || 0;
        this.bytes = bytesOf(address, family);
      }
      detail(out) {
        out.address = this.family === AF_INET ? this.bytes.join(".") : hn.net.format6(this.bytes);
        out.port = this.port;
        out.family = this.family;
        out.flowlabel = this.label;
        return out;
      }
      flowlabel() {
        return this.label;
      }
    }
    class BlockList {
      constructor() {
        this.rules = [];
      }
      addAddress(address) {
        this.rules.push({ text: "Address: " + (address.family === AF_INET ? "IPv4 " : "IPv6 ") + address.detail({}).address,
                          test: (a) => compare(a, wide(address)) === 0 });
      }
      addRange(start, end) {
        if (compare(wide(start), wide(end)) > 0) {
          return false;
        }
        const family = start.family === AF_INET ? "IPv4 " : "IPv6 ";
        this.rules.push({ text: "Range: " + family + start.detail({}).address + "-" + end.detail({}).address,
                          test: (a) => compare(a, wide(start)) >= 0 && compare(a, wide(end)) <= 0 });
        return true;
      }
      addSubnet(network, prefix) {
        const net = wide(network);
        const bits = prefix + (network.family === AF_INET ? 96 : 0);
        this.rules.push({
          text: "Subnet: " + (network.family === AF_INET ? "IPv4 " : "IPv6 ") + network.detail({}).address + "/" + prefix,
          test: (a) => {
            for (let i = 0; i < bits; i++) {
              const mask = 0x80 >> (i & 7);
              if ((a[i >> 3] & mask) !== (net[i >> 3] & mask)) {
                return false;
              }
            }
            return true;
          },
        });
      }
      check(address) {
        const a = wide(address);
        return this.rules.some((rule) => rule.test(a));
      }
      getRules() {
        return this.rules.map((rule) => rule.text).reverse();
      }
    }
    return { BlockList, SocketAddress, AF_INET, AF_INET6 };
  });
  // HTTP/2 (nghttp2 in node) is not here: the module loads, and says so when a session is made.
  hn.binding("http2", () => hn.complete("http2", { refreshDefaultSettings() {}, setCallbackFunctions() {} }));

  // --- os (src/node_os.cc)
  hn.binding("os", () => {
    const uname = () => {
      sys1(SYS.uname, S);
      const field = (i) => hn.cstring(S + 65 * i);
      return { sysname: field(0), nodename: field(1), release: field(2), version: field(3), machine: field(4) };
    };
    const sysinfo = () => {
      if (sys1(SYS.sysinfo, S) < 0) {
        return null;
      }
      const v = dv();
      const unit = v.getUint32(S + 104, true) || 1;
      return {
        uptime: Number(v.getBigInt64(S, true)),
        loads: [0, 1, 2].map((i) => Number(v.getBigUint64(S + 8 + 8 * i, true)) / 65536),
        total: Number(v.getBigUint64(S + 32, true)) * unit,
        free: Number(v.getBigUint64(S + 40, true)) * unit,
      };
    };
    const passwd = (uid) => {
      const file = hn.readText("/etc/passwd") || "";
      for (const line of file.split("\n")) {
        const f = line.split(":");
        if (f.length >= 7 && Number(f[2]) === uid) {
          return { username: f[0], uid: Number(f[2]), gid: Number(f[3]), homedir: f[5], shell: f[6] };
        }
      }
      return null;
    };
    const cpus = () => {
      const r = sys3(SYS.sched_getaffinity, 0, 128, S);
      let n = 0;
      for (let i = 0; i < r; i++) {
        for (let b = u8()[S + i]; b; b >>= 1) {
          n += b & 1;
        }
      }
      return Math.max(1, n);
    };
    return {
      getHostname: () => uname().nodename,
      getOSInformation: () => {
        const u = uname();
        return [u.sysname, u.version, u.release, u.machine];
      },
      getLoadAvg(array) {
        const s = sysinfo();
        for (let i = 0; i < 3; i++) {
          array[i] = s ? s.loads[i] : 0;
        }
      },
      getUptime: () => {
        const s = sysinfo();
        return s ? s.uptime : 0;
      },
      getTotalMem: () => {
        const s = sysinfo();
        return s ? s.total : 0;
      },
      getFreeMem: () => {
        const s = sysinfo();
        return s ? s.free : 0;
      },
      getCPUs() {
        const result = [];
        for (let i = cpus(); i > 0; i--) {
          result.push("the browser's", 0, 0, 0, 0, 0, 0);
        }
        return result;
      },
      getAvailableParallelism: cpus,
      getInterfaceAddresses: () => [],
      getHomeDirectory() {
        if (hn.env.has("HOME")) {
          return hn.env.get("HOME");
        }
        const p = passwd(sys0(SYS.getuid));
        return p ? p.homedir : "/";
      },
      getUserInfo(options, ctx) {
        const uid = sys0(SYS.geteuid);
        const p = passwd(uid) || { username: String(uid), uid, gid: sys0(SYS.getegid), homedir: "/", shell: null };
        return { uid: p.uid, gid: p.gid, username: p.username, homedir: p.homedir, shell: p.shell };
      },
      setPriority(pid, priority, ctx) {
        const r = sys3(SYS.setpriority, 0, pid, priority);
        if (r < 0) {
          hn.fail(ctx, r, "uv_os_setpriority");
          return -1;
        }
        return 0;
      },
      getPriority(pid, ctx) {
        const r = sys2(SYS.getpriority, 0, pid);
        if (r < 0) {
          hn.fail(ctx, r, "uv_os_getpriority");
          return undefined;
        }
        return 20 - r;	// the kernel answers 20 - nice
      },
      isBigEndian: false,
    };
  });

  // --- signal_wrap (src/signal_wrap.cc): a listener for one signal.
  hn.binding("signal_wrap", () => {
    class Signal extends hn.HandleWrap {
      constructor() {
        super("SIGNALWRAP");
        this.__signal = 0;
        this.__handler = null;
      }
      start(signal) {
        if (this.__handler) {
          this.stop();
        }
        if (signal === 9 || signal === 19) {
          return -22;	// not to be caught: EINVAL
        }
        let set = loop.signals.get(signal);
        if (!set) {
          loop.signals.set(signal, (set = new Set()));
          try {
            loop.signalOn(signal);
          } catch (error) {
            loop.signals.delete(signal);
            return -(error.errno || 22);
          }
        }
        this.__signal = signal;
        this.__handler = () => hn.makeCallback(this, () => this.onsignal(signal));
        set.add(this.__handler);
        this.__setActive(true);
        return 0;
      }
      stop() {
        if (this.__handler) {
          const set = loop.signals.get(this.__signal);
          set.delete(this.__handler);
          if (set.size === 0) {
            loop.signals.delete(this.__signal);
            loop.signalOff(this.__signal);
          }
          this.__handler = null;
          this.__setActive(false);
        }
        return 0;
      }
      __close() {
        this.stop();
      }
    }
    return { Signal };
  });
})
