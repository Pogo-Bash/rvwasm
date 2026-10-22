# Compiler flags for the prototype (as nativeqt/bin/nq-clang, which builds static PIEs).
NQ_SYSROOT=${NQ_SYSROOT:-$HOME/rvwasm/nativeqt/sysroot}
NQ_LLVM=${NQ_LLVM:-$HOME/rvwasm/linux-wasm/workspace/install/llvm/bin}
CFLAGS=(--target=wasm64-unknown-unknown --sysroot="$NQ_SYSROOT"
	-D__linux__ -D__unix__ -D__unix -D__USING_WASM_EXCEPTIONS__
	-Xclang -target-feature -Xclang +atomics -Xclang -target-feature -Xclang +bulk-memory
	-Xclang -target-feature -Xclang +mutable-globals -Xclang -target-feature -Xclang +sign-ext
	-fPIC -pthread -fwasm-exceptions -mllvm -wasm-enable-sjlj -O2)
CXXFLAGS=("${CFLAGS[@]}" -nostdinc++ -isystem "$NQ_SYSROOT/usr/include/c++/v1")
CFLAGS+=(-isystem "$NQ_SYSROOT/usr/include")
CXXFLAGS+=(-isystem "$NQ_SYSROOT/usr/include")
CC="$NQ_LLVM/clang"
