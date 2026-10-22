// libxkbcommon.so through the loader: a context and the compositor's keymap, compiled from a string as Qt's Wayland client does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	FILE *f = fopen(argc > 1 ? argv[1] : "/wasmdyn/us.xkb", "r");
	if (!f) { printf("xkbtest: no keymap file\n"); return 2; }
	static char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = 0;
	printf("xkbtest: keymap text %zu bytes\n", n);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	printf("xkbtest: context %p\n", (void *)ctx);
	struct xkb_keymap *km = xkb_keymap_new_from_string(ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	printf("xkbtest: keymap %p\n", (void *)km);
	if (km) {
		struct xkb_state *st = xkb_state_new(km);
		printf("xkbtest: key 38 -> %c\n", (char)xkb_state_key_get_one_sym(st, 38));
	}
	printf("xkbtest: %s\n", km ? "ok" : "FAILED");
	return km ? 0 : 1;
}
