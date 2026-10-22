#!/usr/bin/env python3
# Make vfront's TGSI text parser from Mesa's (src/gallium/auxiliary/tgsi/tgsi_text.c): the guest's virgl
# driver dumps TGSI with TGSI_DUMP_FLOAT_AS_HEX and virglrenderer's parser has a few differences Mesa's lacks.
#   make-tgsi-text.py MESA_SRC OUT   (OUT: vf_tgsi_text.c; run again when Mesa changes)
import sys
src, out = sys.argv[1], sys.argv[2]
s = open(src + '/src/gallium/auxiliary/tgsi/tgsi_text.c').read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit('expected %d of %r, found %d' % (count, old[:60], n))
    s = s.replace(old, new)

header = '''/*
 * vfront's copy of Mesa's TGSI text parser (src/gallium/auxiliary/tgsi/tgsi_text.c, Mesa 25.0.7), made by
 * build/make-tgsi-text.py, with what virglrenderer's own copy of the parser accepts and Mesa's does not,
 * because the guest's virgl driver sends it:
 *  - floats and doubles written as hex (TGSI_DUMP_FLOAT_AS_HEX): "0x3f800000";
 *  - negative register indices in brackets;
 *  - BARRIER is not a memory instruction;
 *  - an unknown property is skipped, not an error.
 * The entry point is vf_tgsi_text_translate().
 */
'''
s = header + s

# hex floats
rep('''static bool parse_float( const char **pcur, float *val )
{
   const char *cur = *pcur;
   *val = _mesa_strtof(cur, (char**)pcur);''', '''static bool parse_float( const char **pcur, float *val )
{
   const char *cur = *pcur;
   if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
      union { uint32_t u; float f; } fi;
      char *end;
      fi.u = (uint32_t)strtoul(cur, &end, 16);
      if (end == cur)
         return false;
      *val = fi.f;
      *pcur = end;
      return true;
   }
   *val = _mesa_strtof(cur, (char**)pcur);''')

# hex doubles: "0x%08x0x%08x" (low then high dword, as tgsi_dump writes them)
i = s.index('static bool parse_double(')
j = s.index('{', i)
body_start = j + 1
rep('''   v.dval = _mesa_strtod(cur, (char**)pcur);''', '''   if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
      char *end;
      *val0 = (uint32_t)strtoul(cur, &end, 16);
      if (end == cur || end[0] != '0' || (end[1] != 'x' && end[1] != 'X'))
         return false;
      cur = end;
      *val1 = (uint32_t)strtoul(cur, &end, 16);
      if (end == cur)
         return false;
      *pcur = end;
      return true;
   }
   v.dval = _mesa_strtod(cur, (char**)pcur);''')

# signed bracket indices
rep('''      if (!parse_uint( &ctx->cur, &uindex )) {
         report_error( ctx, "Expected literal unsigned integer" );
         return false;
      }
      brackets->index = (int) uindex;''', '''      int sindex;
      if (!parse_int( &ctx->cur, &sindex )) {
         report_error( ctx, "Expected literal integer" );
         return false;
      }
      (void)uindex;
      brackets->index = sindex;''')

# BARRIER
rep('''   if ((i >= TGSI_OPCODE_LOAD && i <= TGSI_OPCODE_ATOMIMAX) ||''',
    '''   if ((i >= TGSI_OPCODE_LOAD && i <= TGSI_OPCODE_ATOMIMAX && i != TGSI_OPCODE_BARRIER) ||''')

# unknown property: skip the line
rep('''   if (property_name >= TGSI_PROPERTY_COUNT) {
      debug_printf( "\\nError: Unknown property : '%s'", id );
      return false;
   }''', '''   if (property_name >= TGSI_PROPERTY_COUNT) {
      debug_printf( "\\nvfront: unknown TGSI property '%s' skipped\\n", id );
      while (*ctx->cur != '\\0' && *ctx->cur != '\\n')
         ctx->cur++;
      return true;
   }''')

# the entry point
rep('''\ntgsi_text_translate(''', '''\nvf_tgsi_text_translate(''')
rep('''#include "tgsi_dump.h"\n''', '''#include "tgsi_dump.h"\n\nbool vf_tgsi_text_translate(const char *text, struct tgsi_token *tokens, unsigned num_tokens);\n''')
s = s.replace('#include "tgsi_text.h"\n', '#include "tgsi/tgsi_text.h"\n')
s = s.replace('#include "tgsi_build.h"\n', '#include "tgsi/tgsi_build.h"\n')
s = s.replace('#include "tgsi_info.h"\n', '#include "tgsi/tgsi_info.h"\n')
s = s.replace('#include "tgsi_parse.h"\n', '#include "tgsi/tgsi_parse.h"\n')
s = s.replace('#include "tgsi_sanity.h"\n', '#include "tgsi/tgsi_sanity.h"\n')
s = s.replace('#include "tgsi_util.h"\n', '#include "tgsi/tgsi_util.h"\n')
s = s.replace('#include "tgsi_dump.h"\n', '#include "tgsi/tgsi_dump.h"\n')
s = s.replace('#include "tgsi_strings.h"\n', '#include "tgsi/tgsi_strings.h"\n')
s = s.replace('#include "tgsi_text.h"', '#include "tgsi/tgsi_text.h"')
s += '''
'''
open(out, 'w').write(s)
print('ok', len(s))
