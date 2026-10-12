#!/bin/bash
# The tools a developer's day is made of, each tried once on the Debian
# system: does it install, start, and do one real thing? Run inside Debian,
# with the network up:
#
#   cp /rv/devtools.sh /debian/root/; debian "bash /root/devtools.sh"     (on Linux/Wasm)
#
# One line per step:   DEV <name> ok|FAILED <milliseconds>: <last line of its output>
# and after a failure, the end of what it printed. `devtools.sh NAME...` runs only those steps.

cd "$(mktemp -d /root/devtools.XXXXXX)" || exit 1
export DEBIAN_FRONTEND=noninteractive HOME=/root
want=" $* "

step() {
	local name=$1 start end out rc
	shift
	[ "$want" = "  " ] || [[ "$want" == *" $name "* ]] || return 0
	start=$(date +%s%N)
	out=$("$@" 2>&1)
	rc=$?
	end=$(date +%s%N)
	echo "DEV $name $([ $rc = 0 ] && echo ok || echo "FAILED($rc)") $(( (end - start) / 1000000 )): $(echo "$out" | tail -n 1 | cut -c1-100)"
	if [ $rc != 0 ]; then
		# The beginning and the end of what it said.
		if [ "$(echo "$out" | wc -l)" -gt 45 ]; then
			echo "$out" | head -n 18 | cut -c1-200 | sed 's/^/    | /'
			echo "    | ..."
		fi
		echo "$out" | tail -n 25 | cut -c1-200 | sed 's/^/    | /'
	fi
}

install() {
	apt-get install -y -qq --no-install-recommends "$@" > apt.log 2>&1 || { tail -n 20 apt.log; return 1; }
	echo "installed: $*"
}

t_c() {
	printf '#include <stdio.h>\nint main(void) { printf("c %%s\\n", __VERSION__); return 0; }\n' > h.c
	gcc -O2 h.c -o h && ./h
}
t_cxx() {
	printf '#include <iostream>\n#include <map>\n#include <string>\n#include <thread>\nint main() { std::map<std::string, int> m{{"c++", 20}}; std::thread t([&]{ m["threads"] = 1; }); t.join(); std::cout << "c++ " << m.size() << "\\n"; }\n' > h.cc
	g++ -O2 -std=c++20 h.cc -o hxx && ./hxx
}
t_make() {
	rm -rf kilo && git clone -q --depth 1 https://github.com/antirez/kilo && make -s -C kilo && ls -la kilo/kilo | cut -c1-60
}
t_git() {
	rm -rf repo && git init -q repo && cd repo && git config user.email a@b.c && git config user.name t &&
		echo one > f && git add f && git commit -q -m one && echo two >> f && git commit -q -am two &&
		git log --oneline | wc -l && cd ..
}
t_rust() {
	printf 'use std::thread;\nfn main() { let h: Vec<_> = (0..4u64).map(|i| thread::spawn(move || (1..=20).map(|x| x + i).sum::<u64>())).collect(); let s: u64 = h.into_iter().map(|t| t.join().unwrap()).sum(); println!("rust {}", s); }\n' > r.rs
	rustc -O r.rs && ./r
}
t_cargo() {
	rm -rf hello && cargo new -q hello && cd hello && cargo build -q --release 2>&1 && ./target/release/hello && cd ..
}
t_go() {
	printf 'package main\nimport ("fmt"; "sync")\nfunc main() { var wg sync.WaitGroup; c := make(chan int, 8); for i := 0; i < 8; i++ { wg.Add(1); go func(i int) { defer wg.Done(); c <- i * i }(i) }; wg.Wait(); close(c); s := 0; for v := range c { s += v }; fmt.Println("go", s) }\n' > g.go
	go run g.go
}
t_java() {
	printf 'public class J { public static void main(String[] a) throws Exception { Thread t = new Thread(() -> System.out.println("java " + Runtime.version().feature())); t.start(); t.join(); } }\n' > J.java
	java J.java
}
t_python() {
	python3 -c 'import json, hashlib, concurrent.futures as f; print("python", sum(f.ThreadPoolExecutor(4).map(lambda i: i * i, range(100))), hashlib.sha256(json.dumps([1, 2]).encode()).hexdigest()[:8])'
}
t_venv() {
	rm -rf v && python3 -m venv v && v/bin/pip install -q requests && v/bin/python -c 'import requests; print("pip requests", requests.__version__, requests.get("https://example.com").status_code)'
}
t_sqlite() {
	# The write-ahead log: its index is a file that processes map shared and writable.
	rm -f t.db* && python3 - <<'END'
import sqlite3, subprocess, sys
c = sqlite3.connect("t.db")
mode = c.execute("pragma journal_mode=wal").fetchone()[0]
c.execute("create table t(a)")
c.executemany("insert into t values (?)", [(i,) for i in range(1000)])
c.commit()
other = subprocess.run([sys.executable, "-c", "import sqlite3; c = sqlite3.connect('t.db'); print(c.execute('select count(*), sum(a) from t').fetchone()); c.execute('insert into t values (5000)'); c.commit()"], capture_output=True, text=True)
print("sqlite", mode, other.stdout.strip(), other.stderr.strip()[-200:], c.execute("select count(*), sum(a) from t").fetchone())
sys.exit(0 if mode == "wal" and other.returncode == 0 and c.execute("select sum(a) from t").fetchone()[0] == 504500 else 1)
END
}
t_node() {
	node -e 'const { Worker } = require("worker_threads"); new Worker("require(\"worker_threads\").parentPort.postMessage(6 * 7)", { eval: true }).on("message", (m) => console.log("node", process.version, m));'
}
t_npm() {
	rm -rf n && mkdir n && cd n && npm init -y > /dev/null && npm install --no-fund --no-audit --no-progress left-pad > /dev/null 2>&1 &&
		node -e 'console.log(require("left-pad")("npm", 6))' && cd ..
}
t_ssh() {
	rm -f k k.pub && ssh-keygen -q -t ed25519 -N "" -f k && ssh-keygen -l -f k.pub | cut -c1-40
}
t_curl() {
	curl -sS -o /dev/null -w 'https %{http_code} %{time_total}s\n' https://deb.debian.org/debian/dists/trixie/InRelease
}

step apt-update apt-get update -qq
step install-c install gcc g++ libc6-dev make
step c t_c
step c++ t_cxx
step git t_git
step make t_make
step python t_python
step sqlite-wal t_sqlite
step curl t_curl
step install-ssh install openssh-client
step ssh-keygen t_ssh
step install-venv install python3-venv
step venv-pip t_venv
step node t_node
step npm t_npm
step install-rust install rustc cargo
step rust t_rust
step cargo t_cargo
step install-go install golang-go
step go t_go
step install-java install default-jdk-headless
step java t_java
echo "DEV done"
