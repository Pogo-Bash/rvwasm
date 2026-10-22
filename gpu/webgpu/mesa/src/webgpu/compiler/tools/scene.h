/*
 * The draw a scene test makes (tests/scene/NAME.scene), read by wgsl-corpus -scene (the shader key: vertex
 * input, which bindings are pulled, point expansion) and by scene-run (the draw on both sides). Lines:
 *
 *   draw plain|indexed16|indexed32 count N instances N first N base_vertex N first_instance N [indirect]
 *   topology triangles|points|lines|line_strip|triangle_strip
 *   push BYTES                    push constants (word i = 0x3c000000 + i * 0x00100000: small floats)
 *   pull all                      pull every vertex buffer, not only those WebGPU cannot express
 *   sampler nearest|linear        every sampler (clamp to edge); default nearest
 *   vb B stride S rate vertex|instance divisor D data bytes|floats
 *   attr L binding B format NAME offset O      (NAME: a PIPE_FORMAT_ name without the prefix)
 *   ubo SET BINDING W0 W1 ...     the words of that uniform buffer (floats; "i:N" an integer; "id4" a
 *                                 4x4 identity matrix); others have the default pattern
 *   buf SET BINDING ELEM OFF W... words of a uniform or storage buffer (array element ELEM) from word OFF;
 *                                 a buffer with any of these starts zeroed
 *   xfb B size BYTES              transform feedback buffer B (captured on both sides and compared)
 *   pushw W0 W1 ...               push constant words (integers), the rest the default pattern
 *   spirv STAGE PATH              a stage (vert, geom, frag) from SPIR-V (PATH under $W/out, no .spv): the
 *                                 Blender corpus; read by scene-check.sh only
 *
 * Positions should come from gl_VertexIndex / gl_InstanceIndex; vertex data is a byte pattern ("bytes")
 * or floats in [-1, 1] ("floats"). Index i of the index buffer is (i * 7) % 24.
 */
#ifndef SCENE_H
#define SCENE_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SCENE_BUF_WORDS 4096
struct scene {
   bool valid, indexed, points, pull_all, linear, indirect;
   int topology;                 /* VkPrimitiveTopology */
   unsigned patch_size;          /* topology patches N */
   unsigned index_bits, count, instances, first, base_vertex, first_instance, push_bytes;
   struct { bool used, per_instance, floats; unsigned stride, divisor; } vb[8];
   struct { bool used; unsigned binding, offset; char format[48]; } attr[16];
   struct scene_ubo { bool used; unsigned set, binding, elem, n; uint32_t w[SCENE_BUF_WORDS]; } ubo[16];
   uint32_t push_w[32], push_set;    /* pushw: push constant words given */
   unsigned view;                    /* view N: multiview, the draw renders view N only (layer N) */
   unsigned depth_bits, depth_op;    /* depth d16|d32 OP: a read-only depth attachment (d16: a pattern,
                                        d32: cleared to 0.5) tested with VkCompareOp OP, no depth writes */
   unsigned layer;                   /* layer N: a layered pass of N + 1 layers, layer N compared */
   unsigned nviewports;              /* viewport I X Y W H MIN MAX / scissor I X Y W H: multiple viewports */
   float viewport[16][6];
   int scissor[16][4];
   unsigned xfb_size[4];             /* xfb B size BYTES: transform feedback buffers */
};

static unsigned
scene_num(const char *line, const char *key, unsigned def)
{
   const char *p = strstr(line, key);
   return p ? (unsigned)strtoul(p + strlen(key), NULL, 0) : def;
}

static bool
scene_load(const char *path, struct scene *sc)
{
   memset(sc, 0, sizeof(*sc));
   sc->count = 6;
   sc->instances = 1;
   sc->topology = 3;
   FILE *f = fopen(path, "r");
   if (!f)
      return false;
   char l[512];
   while (fgets(l, sizeof(l), f)) {
      unsigned a, b;
      char word[64], fmt[64];
      if (!strncmp(l, "draw ", 5)) {
         sc->indexed = strstr(l, "indexed") != NULL;
         sc->indirect = strstr(l, " indirect") != NULL;
         sc->index_bits = strstr(l, "indexed16") ? 16 : 32;
         sc->count = scene_num(l, " count ", 6);
         sc->instances = scene_num(l, " instances ", 1);
         sc->first = scene_num(l, " first ", 0);
         sc->base_vertex = scene_num(l, " base_vertex ", 0);
         sc->first_instance = scene_num(l, " first_instance ", 0);
      } else if (!strncmp(l, "topology ", 9)) {
         sc->points = strstr(l, "points") != NULL;
         sc->topology = sc->points ? 0 : strstr(l, "line_strip") ? 2 : strstr(l, "lines") ? 1 :
                        strstr(l, "triangle_strip") ? 4 : strstr(l, "patches") ? 10 : 3;
         if (sc->topology == 10)
            sc->patch_size = scene_num(l, "patches ", 3);
      } else if (!strncmp(l, "push ", 5)) {
         sc->push_bytes = strtoul(l + 5, NULL, 0);
      } else if (!strncmp(l, "pull all", 8)) {
         sc->pull_all = true;
      } else if (!strncmp(l, "sampler ", 8)) {
         sc->linear = strstr(l, "linear") != NULL;
      } else if (sscanf(l, "vb %u stride %u rate %63s", &a, &b, word) == 3 && a < 8) {
         sc->vb[a].used = true;
         sc->vb[a].stride = b;
         sc->vb[a].per_instance = !strcmp(word, "instance");
         sc->vb[a].divisor = scene_num(l, " divisor ", 1);
         sc->vb[a].floats = strstr(l, "data floats") != NULL;
      } else if (!strncmp(l, "ubo ", 4) || !strncmp(l, "buf ", 4)) {
         /* ubo SET BINDING W...  /  buf SET BINDING ELEMENT OFFSET W... (OFFSET in words) */
         bool full = l[0] == 'b';
         char *p = l + 4;
         unsigned set = strtoul(p, &p, 0), binding = strtoul(p, &p, 0);
         unsigned elem = full ? strtoul(p, &p, 0) : 0, off = full ? strtoul(p, &p, 0) : 0;
         struct scene_ubo *u = NULL;
         for (unsigned k = 0; k < 16 && !u; k++)
            if (sc->ubo[k].used && sc->ubo[k].set == set && sc->ubo[k].binding == binding && sc->ubo[k].elem == elem)
               u = &sc->ubo[k];
         for (unsigned k = 0; k < 16 && !u; k++)
            if (!sc->ubo[k].used) {
               u = &sc->ubo[k];
               u->used = true;
               u->set = set;
               u->binding = binding;
               u->elem = elem;
            }
         if (!u)
            continue;
         unsigned n = off;
         while (n < SCENE_BUF_WORDS) {
            while (*p == ' ' || *p == '\t')
               p++;
            if (!*p || *p == '\n' || *p == '#')
               break;
            if (p[0] == 'i' && p[1] == ':') {
               u->w[n++] = (uint32_t)strtol(p + 2, &p, 0);
            } else if (!strncmp(p, "id4", 3)) {
               /* a 4x4 identity matrix */
               for (unsigned k = 0; k < 16 && n < SCENE_BUF_WORDS; k++) {
                  float f = k % 5 == 0 ? 1.0f : 0.0f;
                  memcpy(&u->w[n++], &f, 4);
               }
               p += 3;
            } else {
               float f = strtof(p, &p);
               memcpy(&u->w[n++], &f, 4);
            }
         }
         u->n = u->n > n ? u->n : n;
      } else if (!strncmp(l, "viewport ", 9)) {
         float f[6];
         if (sscanf(l + 9, "%u %f %f %f %f %f %f", &a, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) == 7 && a < 16) {
            memcpy(sc->viewport[a], f, sizeof(f));
            if (a + 1 > sc->nviewports) {
               for (unsigned k = sc->nviewports; k < a; k++) {
                  float d[6] = { 0, 0, 64, 64, 0, 1 };
                  memcpy(sc->viewport[k], d, sizeof(d));
               }
               for (unsigned k = sc->nviewports; k <= a; k++) {
                  sc->scissor[k][0] = sc->scissor[k][1] = 0;
                  sc->scissor[k][2] = sc->scissor[k][3] = 64;
               }
               sc->nviewports = a + 1;
            }
         }
      } else if (!strncmp(l, "scissor ", 8)) {
         int r[4];
         if (sscanf(l + 8, "%u %d %d %d %d", &a, &r[0], &r[1], &r[2], &r[3]) == 5 && a < 16)
            memcpy(sc->scissor[a], r, sizeof(r));
      } else if (sscanf(l, "depth d%u %u", &a, &b) == 2 && (a == 16 || a == 32)) {
         sc->depth_bits = a;
         sc->depth_op = b;
      } else if (sscanf(l, "layer %u", &a) == 1 && a < 8) {
         sc->layer = a;
         if (!sc->nviewports) {
            float d[6] = { 0, 0, 64, 64, 0, 1 };
            memcpy(sc->viewport[0], d, sizeof(d));
            sc->scissor[0][2] = sc->scissor[0][3] = 64;
            sc->nviewports = 1;
         }
      } else if (sscanf(l, "view %u", &a) == 1 && a < 8) {
         sc->view = a;
      } else if (sscanf(l, "xfb %u size %u", &a, &b) == 2 && a < 4) {
         sc->xfb_size[a] = b;
      } else if (!strncmp(l, "pushw ", 6)) {
         char *p = l + 6;
         for (unsigned k = 0; k < 32; k++) {
            while (*p == ' ')
               p++;
            if (!*p || *p == '\n')
               break;
            sc->push_w[k] = (uint32_t)strtoul(p, &p, 0);
            sc->push_set |= 1u << k;
         }
      } else if (sscanf(l, "attr %u binding %u format %63s", &a, &b, fmt) == 3 && a < 16) {
         sc->attr[a].used = true;
         sc->attr[a].binding = b;
         sc->attr[a].offset = scene_num(l, " offset ", 0);
         snprintf(sc->attr[a].format, sizeof(sc->attr[a].format), "%s", fmt);
      }
   }
   fclose(f);
   sc->valid = true;
   return true;
}

/* the vertex buffer contents: 4096 bytes per binding */
#define SCENE_VB_SIZE 4096
static void
scene_vb_data(const struct scene *sc, unsigned b, unsigned char *out)
{
   for (unsigned k = 0; k < SCENE_VB_SIZE; k++)
      out[k] = (unsigned char)(k * 29 + b * 71 + 13);
   if (sc->vb[b].floats)
      for (unsigned k = 0; k < SCENE_VB_SIZE / 4; k++) {
         float v = ((int)(k * 37 % 41) - 20) / 20.0f;
         memcpy(out + 4 * k, &v, 4);
      }
}

/* the scene's words for uniform buffer (set, binding), or NULL */
static const struct scene_ubo *
scene_ubo(const struct scene *sc, unsigned set, unsigned binding, unsigned elem)
{
   for (unsigned k = 0; k < 16; k++)
      if (sc->ubo[k].used && sc->ubo[k].set == set && sc->ubo[k].binding == binding && sc->ubo[k].elem == elem)
         return &sc->ubo[k];
   return NULL;
}

static unsigned
scene_index(unsigned i)
{
   return (i * 7) % 24;
}

/* the d16 depth pattern: texel (x, y) */
static unsigned short
scene_depth16(unsigned x, unsigned y)
{
   return (unsigned short)(((x * 5 + y * 3) % 64) * 1024 + 512);
}

static unsigned
scene_push_word(unsigned i)
{
   return 0x3c000000u + i * 0x00100000u;
}
#endif
