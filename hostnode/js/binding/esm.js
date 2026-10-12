// module_wrap (src/module_wrap.cc): ES modules. In node a ModuleWrap is a
// module of V8's, which C++ compiles, links to the modules it imports and
// evaluates, with node's own loader (lib/internal/modules/esm) deciding what
// each import means. A page's JavaScript cannot do the three steps: all it
// has is import() of an address, and then the browser does everything,
// asking nobody what the imports mean.
//
// So the loader keeps deciding, and what the engine gets is each module's
// text with its imports already decided: every module becomes a blob with an
// address of its own, and where its text names another module, the address
// of that module's blob is put in. To that end the text is looked through
// for its imports (es-module-lexer, as WebAssembly, taken from the guest
// system by hostnode/extract.js), and three things are changed in it:
//
//   import ... from "x"     "x" becomes the address of the blob for what the loader resolved it to
//   import.meta             becomes an object that the loader fills in (url, dirname, resolve...)
//   import(x)               becomes a call into the loader
//
// A module that is not text (a built-in module of node's, a CommonJS module
// seen from an ES module, JSON) is a blob too: a few lines that export
// variables which node sets.
//
// What does not work: modules that import each other in a circle (each
// would need the other's address first), and require() of an ES module.
(function (hn) {
  "use strict";

  const { web, loop } = hn;
  const shape = hn.data.bindings.module_wrap;
  const { kUninstantiated, kInstantiated, kEvaluating, kEvaluated, kErrored } = shape;
  const { host_defined_option_symbol } = hn.privateSymbols;
  const KEY = Symbol.for("hostnode.esm");
  const REGISTRY = 'globalThis[Symbol.for("hostnode.esm")]';

  // --- Looking through text for imports. The lexer starts with a promise (it compiles itself); given
  // promises that are kept at once, it is ready when it has been loaded.
  let lexer = null;
  const parse = (source, name) => {
    if (lexer === null) {
      const text = hn.readText(hn.home + "/es-module-lexer.cjs");
      if (text === null) {
        throw new Error("hostnode: ES modules need es-module-lexer, which is not in " + hn.home +
          " (hostnode/extract.js takes it from the guest system's node-es-module-lexer)");
      }
      const kept = (value) => (value && typeof value.then === "function" ? value : { then: (f) => kept(f(value)) });
      const exports = {};
      new Function("exports", "WebAssembly", text)(exports, {
        compile: (bytes) => kept(new WebAssembly.Module(bytes)),
        instantiate: (module, imports) => kept(new WebAssembly.Instance(module, imports)),
      });
      lexer = exports.parse;
    }
    return lexer(source, name);
  };

  // --- Modules, by number: what a blob's text refers to itself and its loader by.
  const modules = new Map();
  let count = 0;
  // Scripts and functions that are not modules but may import(): [symbol of the referrer, its name].
  const scripts = [];

  const dynamicImport = (symbol, name, specifier, options) => {
    // (Like the engine: whatever goes wrong becomes the promise's failure.)
    try {
      const attributes = { __proto__: null, ...(options && (options.with || options.assert)) };
      return Promise.resolve(hn.importModuleDynamically(symbol, `${specifier}`, attributes, name));
    } catch (error) {
      return Promise.reject(error);
    }
  };
  Object.defineProperty(globalThis, KEY, {
    enumerable: false,
    value: {
      meta(id) {
        const wrap = modules.get(id);
        if (!wrap.__meta) {
          wrap.__meta = { __proto__: null };
          hn.initializeImportMeta(wrap[host_defined_option_symbol], wrap.__meta, wrap);
        }
        return wrap.__meta;
      },
      dynamic(id, specifier, options) {
        const wrap = modules.get(id);
        return dynamicImport(wrap[host_defined_option_symbol], wrap.url, specifier, options);
      },
      script(index, specifier, options) {
        return dynamicImport(scripts[index][0], scripts[index][1], specifier, options);
      },
      synthetic: (id) => modules.get(id),
    },
  });

  // A blob for a module's text, or under Node.js (tools/node-machine.js), which imports no blobs, the text
  // itself as an address.
  const addressOf = (text) => {
    if (hn.dataModules) {
      const bytes = hn.encoder.encode(text);
      return "data:text/javascript;base64," + hn.internalBinding("buffer").base64Slice.call(bytes, 0, bytes.length);
    }
    return web.URL.createObjectURL(new web.Blob([text], { type: "text/javascript" }));
  };
  const sourceURL = (url) => "\n//# sourceURL=" + (/[\s'"]/.test(url) ? encodeURI(url) : url);

  // An import() in a script or function, which is not a module: put through to the loader as well.
  hn.transformScript = (code, filename, symbol) => {
    if (!/\bimport\s*(\/\*[^]*?\*\/\s*)?\(/.test(code)) {
      return code;
    }
    let imports;
    try {
      imports = parse(code, filename)[0];
    } catch (error) {
      return code;	// not something the lexer can read: the engine will say what is wrong with it, if anything
    }
    const dynamic = imports.filter((i) => i.d >= 0);
    if (!dynamic.length) {
      return code;
    }
    const index = scripts.push([symbol || hn.internalBinding("symbols").vm_dynamic_import_default_internal, filename]) - 1;
    let out = "", at = 0;
    for (const i of dynamic) {
      // "import", what is between it and the bracket, and the bracket.
      out += code.slice(at, i.ss) + REGISTRY + ".script(" + index + "," + code.slice(i.ss + 6, i.d).replace(/[^\n]/g, "");
      at = i.d + 1;
    }
    return out + code.slice(at);
  };

  const identifier = /^[\p{ID_Start}$_][\p{ID_Continue}$‌‍]*$/u;
  const keyOf = (specifier, attributes) => specifier + "\n" + (attributes && attributes.type ? attributes.type : "");

  class ModuleWrap {
    constructor(url, context, source, a, b, c) {
      this.url = url;
      this.__id = ++count;
      modules.set(this.__id, this);
      this.__status = kUninstantiated;
      this.__address = null;
      this.__error = undefined;
      this.sourceMapURL = undefined;
      this.async = false;
      if (typeof source === "string") {
        this.__source = source;
        this[host_defined_option_symbol] = typeof c === "symbol" ? c : Symbol(url);
        const [imports] = parse(source, url);
        this.__imports = imports;
        this.__requests = [];
        const seen = new Set();
        for (const i of imports) {
          if (i.d !== -1) {
            continue;
          }
          // `with { type: "json" }` after the statement (the lexer on the system knows only `assert`).
          const rest = /^\s*(?:with|assert)\s*\{([^}]*)\}/.exec(source.slice(i.se, i.se + 200));
          i.attributes = { __proto__: null };
          if (rest) {
            i.clause = rest[0].length;
            for (const pair of rest[1].split(",")) {
              const m = /^\s*["']?(\w+)["']?\s*:\s*["']([^"']*)["']\s*$/.exec(pair);
              if (m) {
                i.attributes[m[1]] = m[2];
              }
            }
          }
          const key = keyOf(i.n, i.attributes);
          if (!seen.has(key)) {
            seen.add(key);
            this.__requests.push({ specifier: i.n, attributes: i.attributes });
          }
        }
        this.__linked = new Map();
      } else {
        // Not text: names to export, and a function that sets them.
        this.__names = source;
        this.__steps = a;
        this.__values = new Map();
        this.__setter = null;
        this.__ran = false;
      }
    }
    getModuleRequests() {
      return this.__requests ? this.__requests.map((r) => ({ specifier: r.specifier, attributes: r.attributes })) : [];
    }
    getStaticDependencySpecifiers() {
      return this.__requests ? this.__requests.map((r) => r.specifier) : [];
    }
    link(specifiers, wraps) {
      for (let i = 0; i < specifiers.length; i++) {
        this.__linked.set(keyOf(specifiers[i], this.__requests[i] && this.__requests[i].attributes), wraps[i]);
      }
    }
    // The address of this module's blob: made when first asked for, the blobs of what it imports before it.
    __build(path) {
      if (this.__address !== null) {
        return this.__address;
      }
      if (path.includes(this)) {
        throw new Error("hostnode: ES modules that import each other in a circle are not supported: " +
          [...path.slice(path.indexOf(this)), this].map((w) => w.url).join(" -> "));
      }
      path.push(this);
      let text;
      if (this.__source === undefined) {
        const names = this.__names;
        text = "const M=" + REGISTRY + ".synthetic(" + this.__id + ");M.__run();" +
          names.map((name, i) => "let $" + i + "=M.__values.get(" + JSON.stringify(name) + ");").join("") +
          "M.__setter=(i,v)=>{switch(i){" + names.map((name, i) => "case " + i + ":$" + i + "=v;break;").join("") + "}};" +
          (names.length ? "export{" + names.map((name, i) => "$" + i + " as " + (identifier.test(name) ? name : JSON.stringify(name))).join(",") + "};" : "") +
          sourceURL(this.url);
      } else {
        const source = this.__source;
        let out = "const __hn_meta__=" + REGISTRY + ".meta(" + this.__id + "),__hn_import__=(s,o)=>" + REGISTRY +
          ".dynamic(" + this.__id + ",s,o);";
        let at = 0;
        if (source.charCodeAt(0) === 35 && source.charCodeAt(1) === 33) {
          at = source.indexOf("\n");	// the line for the interpreter of scripts gives way to ours
          if (at < 0) {
            at = source.length;
          }
        }
        for (const i of this.__imports) {
          if (i.d === -1) {
            const wrap = this.__linked.get(keyOf(i.n, i.attributes));
            if (!wrap) {
              throw new Error("hostnode: " + this.url + " imports " + i.n + ", which was not linked");
            }
            out += source.slice(at, i.s) + wrap.__build(path);
            at = i.e;
            if (i.clause) {
              out += source.slice(at, i.se) + " ".repeat(i.clause);
              at = i.se + i.clause;
            }
          } else if (i.d === -2) {
            out += source.slice(at, i.s) + "__hn_meta__";
            at = i.e;
          } else {
            out += source.slice(at, i.ss) + "__hn_import__";
            at = i.ss + 6;
          }
        }
        text = out + source.slice(at) + sourceURL(this.url);
        this.__source = null;	// (the engine has it now)
        this.__imports = null;
      }
      path.pop();
      this.__address = addressOf(text);
      return this.__address;
    }
    instantiate() {
      if (this.__status === kUninstantiated) {
        this.__build([]);
        this.__status = kInstantiated;
      }
    }
    // Run the function that sets the exports of a module that is not text.
    __run() {
      if (!this.__ran) {
        this.__ran = true;
        this.__status = kEvaluating;
        try {
          this.__steps.call(this);
          this.__status = kEvaluated;
        } catch (error) {
          this.__status = kErrored;
          this.__error = error;
          throw error;
        }
      }
    }
    setExport(name, value) {
      this.__values.set(name, value);
      if (this.__setter) {
        this.__setter(this.__names.indexOf(name), value);
      }
    }
    evaluate(timeout, breakOnSigint) {
      if (this.__names) {
        this.__run();
        return undefined;
      }
      if (!this.__evaluation) {
        this.instantiate();
        this.__status = kEvaluating;
        // The engine fetches, compiles and runs the blobs on its own time: one of the browser's operations.
        loop.hostRef();
        this.__evaluation = import(this.__address).then((namespace) => {
          loop.hostUnref();
          this.__namespace = namespace;
          this.__status = kEvaluated;
        }, (error) => {
          loop.hostUnref();
          this.__status = kErrored;
          this.__error = error;
          throw error;
        });
      }
      return this.__evaluation;
    }
    getNamespace() {
      if (this.__names) {
        if (!this.__namespace) {
          // What importing it gives, without importing it: an object with the same names.
          const namespace = { __proto__: null };
          for (const name of this.__names) {
            Object.defineProperty(namespace, name, { enumerable: true, get: () => this.__values.get(name) });
          }
          Object.defineProperty(namespace, Symbol.toStringTag, { value: "Module" });
          this.__namespace = Object.preventExtensions(namespace);
        }
        return this.__namespace;
      }
      if (!this.__namespace) {
        throw new Error("hostnode: the module has not been evaluated: " + this.url);
      }
      return this.__namespace;
    }
    getStatus() {
      return this.__status;
    }
    getError() {
      return this.__error;
    }
    createCachedData() {
      const { Buffer } = hn.requireBuiltin("buffer");
      return Buffer.alloc(0);
    }
    instantiateSync() {
      throw unsupportedSync(this.url);
    }
    evaluateSync() {
      throw unsupportedSync(this.url);
    }
    getNamespaceSync() {
      throw unsupportedSync(this.url);
    }
  }
  const unsupportedSync = (url) => {
    const error = new Error("hostnode: require() of an ES module is not supported (" + url + "); use import()");
    error.code = "ERR_REQUIRE_ESM";
    return error;
  };
  for (const name of ["__id", "__status", "__address", "__error", "__source", "__imports", "__requests", "__linked",
                      "__names", "__steps", "__values", "__setter", "__ran", "__meta", "__evaluation", "__namespace"]) {
    Object.defineProperty(ModuleWrap.prototype, name, { writable: true, enumerable: false, value: undefined });
  }

  return {
    ModuleWrap,
    setImportModuleDynamicallyCallback(fn) {
      hn.importModuleDynamically = fn;
    },
    setInitializeImportMetaObjectCallback(fn) {
      hn.initializeImportMeta = fn;
    },
    createRequiredModuleFacade(wrap) {
      throw unsupportedSync(wrap && wrap.url);
    },
    kUninstantiated: shape.kUninstantiated,
    kInstantiating: shape.kInstantiating,
    kInstantiated: shape.kInstantiated,
    kEvaluating: shape.kEvaluating,
    kEvaluated: shape.kEvaluated,
    kErrored: shape.kErrored,
  };
})
