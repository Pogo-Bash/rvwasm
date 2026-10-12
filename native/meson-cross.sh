# Sourced: writes the Meson cross file for native linux-wasm programs to $NQ_CROSS.
NQ_CROSS=$NQ/meson-cross.ini
cat > $NQ_CROSS <<EOT
[binaries]
c = '$NQ_TREE/bin/nq-clang'
cpp = '$NQ_TREE/bin/nq-clang++'
ar = '$NQ_LLVM/llvm-ar'
ranlib = '$NQ_LLVM/llvm-ranlib'
strip = '$NQ_LLVM/llvm-strip'
nm = '$NQ_LLVM/llvm-nm'
pkg-config = 'pkg-config'

[properties]
sys_root = '$NQ_SYSROOT'
pkg_config_libdir = ['$NQ_SYSROOT/usr/lib/pkgconfig', '$NQ_SYSROOT/usr/share/pkgconfig']

[built-in options]
default_library = 'static'

[host_machine]
system = 'linux'
cpu_family = 'wasm64'
cpu = 'wasm64'
endian = 'little'
EOT
# (Meson sets PKG_CONFIG_SYSROOT_DIR from sys_root for the cross pkg-config only; exporting it here
# would send the host's pkg-config into the sysroot too.)
