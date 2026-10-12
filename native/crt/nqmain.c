/*
 * main: clang names a Wasm program's `int main(int, char **)` __main_argc_argv
 * and a C++ `int main()` __main_void. linux-wasm's crt1 calls __main_argc_argv,
 * so a program whose main takes nothing needs this bridge. Its own archive
 * member of libnqcrt.a: pulled in only when __main_argc_argv is missing, so that
 * its reference to __main_void is never left undefined (a weak undefined
 * function is an import in a PIE link).
 */
int __main_void(void);

int __main_argc_argv(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return __main_void();
}
