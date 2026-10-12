// Bindings that turn text into running code: contextify (scripts and
// functions, lib/vm.js and the CommonJS loader stand on it) and module_wrap
// (ES modules). In node this is V8's compiler called from C++. Here the
// engine is the browser's, and what a page can ask of it is eval for scripts
// and functions and import() for modules.
(function (hn) {
  "use strict";

  const { web, data } = hn;
  const shapes = data.bindings;

  // The name the engine files code under: what stack traces show.
  const sourceURL = (filename) => {
    filename = String(filename);
    return "\n//# sourceURL=" + (/[\s'"]/.test(filename) ? encodeURI(filename) : filename);
  };
  // A line for the interpreter of scripts is no JavaScript, except at the very start of a script.
  const noHashbang = (code) => (code.charCodeAt(0) === 35 && code.charCodeAt(1) === 33 ? "//" + code.slice(2) : code);

  const esmMessages = [
    "Cannot use import statement outside a module",
    "Unexpected token 'export'",
    "Cannot use 'import.meta' outside a module",
  ];
  const onlyInCommonJS = [
    "Identifier '__filename' has already been declared",
    "Identifier '__dirname' has already been declared",
    "Identifier 'require' has already been declared",
    "Identifier 'module' has already been declared",
    "Identifier 'exports' has already been declared",
    "await is only valid in async functions and the top level bodies of modules",
  ];
  // src/node_contextify.cc: ShouldRetryAsESM.
  const shouldRetryAsESM = (code, error) => {
    const message = String(error && error.message);
    if (esmMessages.some((m) => message.includes(m))) {
      return true;
    }
    if (onlyInCommonJS.some((m) => message.includes(m))) {
      try {
        new Function("(async function() {" + noHashbang(code) + "\n})();");
        return true;
      } catch (inner) {
        return esmMessages.some((m) => String(inner.message).includes(m));
      }
    }
    return false;
  };
  const cjsParameters = "exports, require, module, __filename, __dirname";
  const compile = (parameters, code, filename, symbol) =>
    (0, eval)("(function (" + parameters + ") {" + hn.transformScript(noHashbang(code), filename, symbol) + "\n})" + sourceURL(filename));
  // Text of a script or function, before the engine gets it. (binding/esm.js puts import() right.)
  hn.transformScript = (code) => code;

  hn.binding("contextify", () => {
    const symbols = hn.internalBinding("symbols");
    const { host_defined_option_symbol } = hn.privateSymbols;

    class ContextifyScript {
      constructor(code, filename, lineOffset, columnOffset, cachedData, produceCachedData, parsingContext,
                  hostDefinedOptionId) {
        code = String(code);
        // The engine is asked at once whether this is JavaScript at all, as node does, without running it.
        // (As the body of a function: the one difference is that a function may `return`.)
        new Function(noHashbang(code));
        this[host_defined_option_symbol] = hostDefinedOptionId;
        this.__code = code;
        this.__filename = filename === undefined ? "evalmachine.<anonymous>" : String(filename);
        this.__lineOffset = lineOffset | 0;
        this.cachedDataRejected = cachedData === undefined ? undefined : true;
        this.sourceMapURL = undefined;
      }
      runInContext(sandbox, timeout, displayErrors, breakOnSigint, breakOnFirstLine) {
        const code = "\n".repeat(Math.max(0, this.__lineOffset)) +
          hn.transformScript(noHashbang(this.__code), this.__filename, this[host_defined_option_symbol]) +
          sourceURL(this.__filename);
        if (sandbox === null || sandbox === undefined) {
          return (0, eval)(code);
        }
        return runInSandbox(sandbox, code);
      }
      createCachedData() {
        const { Buffer } = hn.requireBuiltin("buffer");
        return Buffer.alloc(0);
      }
    }

    // Another global object for a script: the engine of a page's worker has one, and no way to make a
    // second. What is given here is the common use of it, a script that reads and sets names in an object:
    // names are looked up in the object first and in the real global object after that.
    const sandboxes = new WeakMap();
    const runInSandbox = (sandbox, code) => {
      let scope = sandboxes.get(sandbox);
      if (!scope) {
        scope = new Proxy(sandbox, {
          has: (target, key) => key !== "eval" && key !== "__hostnode_code__" || key in target,
          get: (target, key) => (key === Symbol.unscopables ? undefined : key in target ? target[key] : globalThis[key]),
          set: (target, key, value) => {
            target[key] = value;
            return true;
          },
        });
        sandboxes.set(sandbox, scope);
      }
      return new Function("__hostnode_scope__", "__hostnode_code__",
        "with (__hostnode_scope__) { return eval(__hostnode_code__); }")(scope, code);
    };

    return hn.complete("contextify", {
      ContextifyScript,
      constants: shapes.contextify.constants,
      makeContext(sandbox) {
        if (sandbox !== null && typeof sandbox === "object") {
          sandbox[hn.privateSymbols.contextify_context_private_symbol] = sandbox;
        }
        return sandbox;
      },
      // lib/internal/vm.js: internalCompileFunction.
      compileFunction(code, filename, lineOffset, columnOffset, cachedData, produceCachedData, parsingContext,
                      contextExtensions, params, hostDefinedOptionId) {
        const fn = compile((params || []).join(", "), "\n".repeat(Math.max(0, lineOffset | 0)) + code, filename, hostDefinedOptionId);
        fn[host_defined_option_symbol] = hostDefinedOptionId;
        return { function: fn, sourceMapURL: undefined, cachedDataRejected: cachedData === undefined ? undefined : true };
      },
      // src/node_contextify.cc: CompileFunctionForCJSLoader.
      compileFunctionForCJSLoader(code, filename, isSeaMain, shouldDetectModule) {
        let fn, canParseAsESM = false;
        try {
          fn = compile(cjsParameters, code, filename);
          fn[host_defined_option_symbol] = symbols.vm_dynamic_import_default_internal;
        } catch (error) {
          if (!(error instanceof SyntaxError) || !shouldDetectModule || !shouldRetryAsESM(code, error)) {
            throw error;
          }
          canParseAsESM = true;
        }
        return { cachedDataRejected: false, sourceMapURL: undefined, function: fn, canParseAsESM };
      },
      containsModuleSyntax(code, filename, resourceName, cjsVar) {
        code = String(code);
        // What makes a module a module is one of these words; without them there is nothing to try.
        if (!/\b(import|export|await)\b/.test(code)) {
          return false;
        }
        try {
          new Function(...(cjsVar === undefined ? cjsParameters.split(", ") : []), noHashbang(code));
          return false;
        } catch (error) {
          return error instanceof SyntaxError && shouldRetryAsESM(code, error);
        }
      },
      shouldRetryAsESM: (code, error) => shouldRetryAsESM(code, error),
      startSigintWatchdog() {},
      stopSigintWatchdog: () => false,
      watchdogHasPendingSigint: () => false,
    });
  });

  hn.binding("module_wrap", () => hn.load("binding/esm.js"));
})
