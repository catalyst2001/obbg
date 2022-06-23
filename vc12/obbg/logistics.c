#include "obbg_funcs.h"
#include "SDL_thread.h"
#include <math.h>


#define TARGET_none    0x7fff
#define TARGET_unknown 0x7ffe

typedef chunk_coord logi_chunk_coord;

typedef struct
{
   uint8 type;
} beltside_item;

typedef struct
{
   uint8 x_off, y_off, z_off; // 3 bytes             // 5,5,3
   uint8 dir  ;               // 1 bytes             // 2
   uint8 len:6;               // 1 bytes             // 5
   int8  turn:2;              // 0 bytes             // 2
   uint8 target_is_neighbor:1;// 1 bytes             // 1
   uint8 type:1;              // 0 bytes             // 1
   uint8 mark:2;              // 0 bytes             // 2
   int8  end_dz:2;            // 0 bytes             // 2
   uint8 mobile_slots[2];     // 2 bytes             // 7,7    // number of slots that can move including frontmost empty slot
   uint16 target_id;          // 2 bytes             // 16
   uint16 target2_id;         // 2 bytes             // 16
   beltside_item *items;      // 4 bytes  (8 bytes in 64-bit)
} belt_run; // 16 bytes

// @TODO add type field to belt run that's large enough to have BR_turn and BR_ramp
enum
{
   BR_normal,
   BR_splitter,
};


#define MAX_UNIQUE_INPUTS 4
typedef struct
{
   logi_chunk_coord pos; // 2 bytes
   uint8 type;           // 1 byte
   uint8 timer;          // 1 byte

   uint8 output;         // 1 byte
   uint8 config;         // 1 byte
   uint8 uses;           // 1 byte
   uint8 input_flags:6;  // 1 byte
   uint8 rot:2;          // 0 bytes
} machine_info;  // 8 bytes

typedef struct
{
   logi_chunk_coord pos;   // 2
   uint8 type;             // 1
   uint8 rot:2;            // 1
   uint8 state:6;          // 1
   uint8 slots[2];         // 2
   uint16 input_id;        // 2
   uint16 output_id[2];    // 4
} belt_machine_info;  // 12 bytes


// 32 * 32 * 8 => 2^(5+5+3) = 2^13
typedef struct
{
   logi_chunk_coord pos;
   uint8 type;
   uint8 rot;
   uint8 state;
   uint8 item;

   uint8 input_is_belt:1;
   uint8 output_is_belt:1;
   uint16 input_id;
   uint16 output_id;
} picker_info; // 12

typedef struct
{
   logi_chunk_coord pos;
   uint16 count;
   uint8 type;
} ore_info; // 8 (5)

enum
{
   M_unmarked,
   M_temporary,
   M_permanent
};

typedef struct
{
   uint8 type[LOGI_CHUNK_SIZE_Z][LOGI_CHUNK_SIZE_Y][LOGI_CHUNK_SIZE_X];
   uint8 rot[LOGI_CHUNK_SIZE_Z][LOGI_CHUNK_SIZE_Y][LOGI_CHUNK_SIZE_X];
   belt_run *belts; // obarr 
   machine_info *machine;
   belt_machine_info *belt_machine;
   picker_info *pickers;
   ore_info *ore;
} logi_chunk;

typedef struct
{
   int slice_x,slice_y;
   logi_chunk *chunk[MAX_Z_POW2CEIL / LOGI_CHUNK_SIZE_Z];
} logi_slice;


#define LOGI_CACHE_SIZE   128

#define LOGI_CHUNK_MASK_X(x) ((x) & (LOGI_CHUNK_SIZE_X-1))
#define LOGI_CHUNK_MASK_Y(y) ((y) & (LOGI_CHUNK_SIZE_Y-1))
#define LOGI_CHUNK_MASK_Z(z) ((z) & (LOGI_CHUNK_SIZE_Z-1))


//   HUGE HACK to keep track of ore for now

stb_define_hash_base(STB_noprefix, stb_uidict, STB_nofields, stb_uidict_,stb_uidict_,0.85f,
              uint32,0,1,STB_nocopy,STB_nodelete,STB_nosafe,
              STB_equal,STB_equal,
              return stb_rehash_improved(k);,
              void *,STB_nullvalue,NULL)

static stb_uidict *uidict;

typedef struct
{
   uint8 z1,z2,type,padding;
   int x,y;
} ore_hack_info;

static ore_hack_info ore_hack[500000];
static ore_hack_info *next_ohi = ore_hack;

static int ore_processed, ore_pending, ore_count;


//   4K x 4K

//     4K/32 => (2^12 / 2^5) => 2^7
//
//  2^7 * 2^7 * 2^5 * 2^2 => 2^(7+7+5+2) = 2^21 = 2MB

static logi_slice logi_world[LOGI_CACHE_SIZE][LOGI_CACHE_SIZE];

static logi_chunk *logistics_get_chunk(int x, int y, int z, logi_slice **r_s)
{
   int slice_x = x >> LOGI_CHUNK_SIZE_X_LOG2;
   int slice_y = y >> LOGI_CHUNK_SIZE_Y_LOG2;
   int chunk_z = z >> LOGI_CHUNK_SIZE_Z_LOG2;
   logi_slice *s = &logi_world[slice_y & (LOGI_CACHE_SIZE-1)][slice_x & (LOGI_CACHE_SIZE-1)];
   if (s->slice_x != slice_x || s->slice_y != slice_y)
      return 0;
   if (r_s != NULL) *r_s = s;
   return s->chunk[chunk_z];
}

static logi_chunk *get_chunk(int x, int y, int z)
{
   return logistics_get_chunk(x,y,z,NULL);
}

static logi_chunk *get_chunk_v(vec3i *v)
{
   return get_chunk(v->x, v->y, v->z);
}

static void logistics_free_chunk(logi_chunk *c)
{
   obarr_free(c->machine);
   obarr_free(c->ore);
   obarr_free(c->pickers);
   obarr_free(c->belts);
   free(c);
}

static void logistics_free_slice(logi_slice *s, int x, int y)
{
   int i;
   for (i=0; i < stb_arrcount(s->chunk); ++i)
      if (s->chunk[i] != NULL)
         logistics_free_chunk(s->chunk[i]);
   memset(s, 0, sizeof(*s));
   s->slice_x = x+1;
}

static void logistics_create_slice(logi_slice *s, int x, int y)
{
   memset(s, 0, sizeof(*s));
   s->slice_x = x;
   s->slice_y = y;
}

extern SDL_mutex *ore_update_mutex;
static logi_chunk *logistics_get_chunk_alloc(int x, int y, int z)
{
   int slice_x = x >> LOGI_CHUNK_SIZE_X_LOG2;
   int slice_y = y >> LOGI_CHUNK_SIZE_Y_LOG2;
   int chunk_z = z >> LOGI_CHUNK_SIZE_Z_LOG2;
   logi_slice *s = &logi_world[slice_y & (LOGI_CACHE_SIZE-1)][slice_x & (LOGI_CACHE_SIZE-1)];
   logi_chunk *c;
   if (s->slice_x != slice_x || s->slice_y != slice_y) {
      logistics_free_slice(s, slice_x, slice_y);
      logistics_create_slice(s, slice_x, slice_y);
   }
   c = s->chunk[chunk_z];
   if (c == NULL) {
      s->chunk[chunk_z] = c = obbg_malloc(sizeof(*c), "/logi/chunk/chunk");
      memset(c, 0, sizeof(*c));
      SDL_LockMutex(ore_update_mutex);
      if (uidict != NULL)
      {
         int bx = slice_x << LOGI_CHUNK_SIZE_X_LOG2;
         int by = slice_y << LOGI_CHUNK_SIZE_Y_LOG2;
         int bz = chunk_z << LOGI_CHUNK_SIZE_Z_LOG2;
         int i,j,k;
         for (j=0; j < LOGI_CHUNK_SIZE_Y; ++j) {
            for (i=0; i < LOGI_CHUNK_SIZE_X; ++i) {
               ore_hack_info *ohi = stb_uidict_get(uidict, (y+j)*65536+(x+i));
               if (ohi != NULL) {
                  if (ohi->z1 < bz+LOGI_CHUNK_SIZE_Z && ohi->z2 > bz) {
                     for (k=ohi->z1; k <= ohi->z2; ++k)
                        if (k >= bz && k < bz+LOGI_CHUNK_SIZE_Z)
                           c->type[k-bz][y-by][x-bx] = ohi->type;
                  }
               }
            }
         }
      }
      SDL_UnlockMutex(ore_update_mutex);
   }
   return c;
}

static int right_offset(belt_run *a)
{
   return 0;
}
static int left_offset(belt_run *a)
{
   return (a)->len * ITEMS_PER_BELT_SIDE;
}

static int offset_for_side(belt_run *a, int side)
{
   if (side == 0)
      return right_offset(a);
   else
      return left_offset(a);
}

static void compute_mobile_slots(belt_run *br)
{
   int side, start, len, end, j;
   if (br->len == 0) return;
   for (side=0; side < 2; ++side) {
      if (br->turn) {
         int right_len = br->turn > 0 ? LONG_SIDE : SHORT_SIDE;
         if (br->turn > 0)
            len = side ? SHORT_SIDE : LONG_SIDE;
         else
            len = side ? LONG_SIDE : SHORT_SIDE;
         start = side ? right_len : 0;
         end = start + len;
      } else {
         start = side ? left_offset(br) : 0;
         len = br->len * ITEMS_PER_BELT_SIDE;
         end = start + len;
      }

      for (j=len-1; j >= 0; --j)
         if (br->items[j].type == 0)
            break;
      br->mobile_slots[0] = j+1;
   }
}

static void split_belt_raw(belt_run *a, belt_run *b, int num_a)
{
   const size_t itemsize = sizeof(a->items[0]);
   int total_len = a->len;
   int old_left_offset = total_len * ITEMS_PER_BELT_SIDE;
   assert(a->end_dz==0);
   assert(num_a <= a->len);
   b->len = a->len - num_a;
   a->len = num_a;

   // 0 1       // 0 5        0 3
   // 2 3       // 1 6        1 4
   // 4 5       // 2 7        2 5
   // 6 7       // 3 8
   // 8 9       // 4 9

   b->items = NULL;
   obarr_setlen(b->items, b->len * ITEMS_PER_BELT, "/logi/beltrun/array");

   // right side
   memcpy(b->items               , a->items                 + a->len*ITEMS_PER_BELT_SIDE, b->len * ITEMS_PER_BELT_SIDE * itemsize);
   // left side
   memcpy(b->items+left_offset(b), a->items+old_left_offset + a->len*ITEMS_PER_BELT_SIDE, b->len * ITEMS_PER_BELT_SIDE * itemsize);

   // move left-side of a's items back to new location for them
   memmove(a->items+left_offset(a), a->items+old_left_offset, a->len*ITEMS_PER_BELT_SIDE);

   obarr_setlen(a->items, a->len * ITEMS_PER_BELT, "/logi/beltrun/array");
}

static int does_belt_intersect(belt_run *a, int x, int y, int z)
{
   if (a->z_off == z) {
      switch (a->dir) {
         case FACE_east:
            if (y == a->y_off && a->x_off <= x && x < a->x_off + a->len)
               return 1;
            break;
         case FACE_west:
            if (y == a->y_off && a->x_off - a->len < x && x <= a->x_off)
               return 1;
            break;
         case FACE_north:
            if (x == a->x_off && a->y_off <= y && y < a->y_off + a->len)
               return 1;
            break;
         case FACE_south:
            if (x == a->x_off && a->y_off - a->len < y && y <= a->y_off)
               return 1;
            break;
      }
   }
   return 0;
}

static void split_belt(int x, int y, int z, int dir)
{
   logi_chunk *c = get_chunk(x,y,z);
   int i, pos;
   x = LOGI_CHUNK_MASK_X(x);
   y = LOGI_CHUNK_MASK_Y(y);
   z = LOGI_CHUNK_MASK_Z(z);
   for (i=0; i < obarr_len(c->belts); ++i) {
      belt_run *a = &c->belts[i];
      if (a->z_off == z && a->dir == dir) {
         switch (dir) {
            case FACE_east:
               if (y == a->y_off && a->x_off <= x && x < a->x_off + a->len) {
                  pos = x - a->x_off;
                  goto found;
               }
               break;
            case FACE_west:
               if (y == a->y_off && a->x_off - a->len < x && x <= a->x_off) {
                  pos = a->x_off - x;
                  goto found;
               }
               break;
            case FACE_north:
               if (x == a->x_off && a->y_off <= y && y < a->y_off + a->len) {
                  pos = y - a->y_off;
                  goto found;
               }
               break;
            case FACE_south:
               if (x == a->x_off && a->y_off - a->len < y && y <= a->y_off) {
                  pos = a->y_off - y;
                  goto found;
               }
               break;
         }
      }
   }
   assert(0);
  found:
   // split belt #i which contains x,y,z at 'pos' offset

   {
      belt_run *e,f,g;

      // split run into e, then f, then g, where 'f' contains x,y,z


      e = &c->belts[i];
      assert(pos >= 0 && pos < e->len);
      assert(e->x_off + face_dir[e->dir][0]*pos == x);
      assert(e->y_off + face_dir[e->dir][1]*pos == y);
      assert(e->end_dz==0);

      if (e->len == 1)
         return;
      e->target_id = TARGET_unknown;

      f.dir = e->dir;
      f.x_off = x;
      f.y_off = y;
      f.z_off = z;
      f.target_id = TARGET_unknown;
      f.end_dz = 0;
      f.turn = 0;

      split_belt_raw(e, &f, pos);
      assert(e->x_off + face_dir[e->dir][0]*e->len == f.x_off);
      assert(e->y_off + face_dir[e->dir][1]*e->len == f.y_off);

      g.dir = f.dir;
      g.x_off = x + face_dir[dir][0];
      g.y_off = y + face_dir[dir][1];
      g.z_off = z;
      g.target_id = TARGET_unknown;
      g.end_dz = 0;
      g.turn = 0;

      assert(f.len >= 1);

      split_belt_raw(&f, &g, 1);
      if (g.len != 0) {
         assert(f.x_off + face_dir[f.dir][0]*f.len == g.x_off);
         assert(f.y_off + face_dir[f.dir][1]*f.len == g.y_off);
      }

      assert(f.len == 1);

      compute_mobile_slots(e);
      compute_mobile_slots(&f);
      compute_mobile_slots(&g);

      if (e->len == 0)
         *e = f;
      else
         obarr_push(c->belts, f, "/logi/beltlist");

      if (g.len != 0)
         obarr_push(c->belts, g, "/logi/beltlist");
   }
}

static void destroy_belt_raw(logi_chunk *c, int belt_id)
{
   belt_run *b = &c->belts[belt_id];
   obarr_free(b->items);
   obarr_fastdelete(c->belts, belt_id);
}

static void destroy_belt(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int i;
   x = LOGI_CHUNK_MASK_X(x);
   y = LOGI_CHUNK_MASK_Y(y);
   z = LOGI_CHUNK_MASK_Z(z);
   for (i=0; i < obarr_len(c->belts); ++i) {
      belt_run *b = &c->belts[i];
      if (b->x_off == x && b->y_off == y && b->z_off == z) {
         assert(b->len == 1);
         destroy_belt_raw(c, i);
         return;
      }
   }
}

static int merge_run(logi_chunk *c, int belt_i, int belt_j)
{
   belt_run *a = &c->belts[belt_i];
   belt_run *b = &c->belts[belt_j];
   int total = a->len + b->len;
   const size_t itemsize = sizeof(a->items[0]);
   assert(a->dir == b->dir);
   assert(a->z_off == b->z_off);
   assert(a->x_off + a->len*face_dir[a->dir][0] == b->x_off);
   assert(a->y_off + a->len*face_dir[a->dir][1] == b->y_off);

   // extend a
   obarr_setlen(a->items, total * ITEMS_PER_BELT, "/logi/beltrun/array");

   // move left-side of a's items forward to new location for them
   memmove(a->items + total*ITEMS_PER_BELT_SIDE, a->items+left_offset(a), a->len*ITEMS_PER_BELT_SIDE*itemsize);

   // copy b's right side to a's right
   memcpy(a->items+a->len*ITEMS_PER_BELT_SIDE               , b->items               , b->len * ITEMS_PER_BELT_SIDE * itemsize);

   // copy b's left side to a's left
   memcpy(a->items+total*ITEMS_PER_BELT_SIDE+a->len*ITEMS_PER_BELT_SIDE, b->items+left_offset(b), b->len * ITEMS_PER_BELT_SIDE * itemsize);

   a->len += b->len;
   a->target_id = TARGET_unknown;

   // delete b
   destroy_belt_raw(c, belt_j);

   // return the merged belt
   if (belt_i == obarr_len(c->belts)) // did i get swapped down over j?
      return belt_j;
   else
      return belt_i;
}

static void create_ramp(int x, int y, int z, int dir, int dz)
{
   logi_chunk *c = get_chunk(x,y,z);
   belt_run nb;
   nb.x_off = (uint8) LOGI_CHUNK_MASK_X(x);
   nb.y_off = (uint8) LOGI_CHUNK_MASK_Y(y);
   nb.z_off = (uint8) LOGI_CHUNK_MASK_Z(z);
   nb.len = 2;
   nb.dir = dir;
   nb.turn = 0;
   nb.items = NULL;
   nb.end_dz = dz;
   nb.target_id = TARGET_unknown;
   obarr_setlen(nb.items, ITEMS_PER_BELT*nb.len, "/logi/beltrun/array");
   memset(nb.items, 0, sizeof(nb.items[0]) * obarr_len(nb.items));
   obarr_push(c->belts, nb, "/logi/beltlist");
}

static void create_splitter(int x, int y, int z, int dir)
{
   logi_chunk *c = get_chunk(x,y,z);
   belt_run nb;
   nb.x_off = (uint8) LOGI_CHUNK_MASK_X(x);
   nb.y_off = (uint8) LOGI_CHUNK_MASK_Y(y);
   nb.z_off = (uint8) LOGI_CHUNK_MASK_Z(z);
   nb.type = BR_splitter;
   nb.len = 1;
   nb.dir = dir;
   nb.turn = 0;
   nb.items = NULL;
   nb.target_id = TARGET_unknown;
   nb.target2_id = TARGET_unknown;
   obarr_setlen(nb.items, ITEMS_PER_BELT*nb.len, "/logi/beltrun/array");
   memset(nb.items, 0, sizeof(nb.items[0]) * obarr_len(nb.items));
   obarr_push(c->belts, nb, "/logi/beltlist");
}

static void destroy_ramp_or_turn(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int i;
   x = LOGI_CHUNK_MASK_X(x);
   y = LOGI_CHUNK_MASK_Y(y);
   z = LOGI_CHUNK_MASK_Z(z);
   for (i=0; i < obarr_len(c->belts); ++i) {
      if (c->belts[i].x_off == x && c->belts[i].y_off == y && c->belts[i].z_off == z) {
         assert(c->belts[i].end_dz != 0 || c->belts[i].turn != 0 || c->belts[i].type != 0);
         destroy_belt_raw(c,i);
         return;
      }
   }
}

static void create_turn(int x, int y, int z, int dir, int turn)
{
   logi_chunk *c = get_chunk(x,y,z);
   belt_run nb;
   nb.x_off = (uint8) LOGI_CHUNK_MASK_X(x);
   nb.y_off = (uint8) LOGI_CHUNK_MASK_Y(y);
   nb.z_off = (uint8) LOGI_CHUNK_MASK_Z(z);
   nb.len = 1;
   nb.dir = dir;
   nb.turn = turn;
   nb.items = NULL;
   nb.end_dz = 0;
   nb.target_id = TARGET_unknown;
   obarr_setlen(nb.items, ITEMS_PER_BELT*nb.len, "/logi/beltrun/array");
   memset(nb.items, 0, sizeof(nb.items[0]) * obarr_len(nb.items));
   obarr_push(c->belts, nb, "/logi/beltlist");
}

// if there is already a belt there, it is one-long
static void create_belt(logi_chunk *c, int x, int y, int z, int dir)
{
   int i, j;
   belt_run *a;
   for (i=0; i < obarr_len(c->belts); ++i)
      if (c->belts[i].x_off == x && c->belts[i].y_off == y && c->belts[i].z_off == z)
         break;
   if (i == obarr_len(c->belts)) {
      belt_run nb;
      nb.x_off = x;
      nb.y_off = y;
      nb.z_off = z;
      nb.len = 1;
      nb.dir = dir;
      nb.items = NULL;
      nb.end_dz = 0;
      nb.turn = 0;
      obarr_setlen(nb.items, 8, "/logi/beltrun/array");
      memset(nb.items, 0, sizeof(nb.items[0]) * obarr_len(nb.items));
      obarr_push(c->belts, nb, "/logi/beltlist");
   } else
      c->belts[i].dir = dir;

   // now i refers to a 1-long belt; check for other belts to merge
   a = &c->belts[i];

   for (j=0; j < obarr_len(c->belts); ++j) {
      if (j != i) {
         belt_run *b = &c->belts[j];
         if (b->end_dz == 0 && b->turn == 0 && b->dir == dir && b->z_off == a->z_off && b->type == BR_normal) {
            if (b->x_off + face_dir[dir][0]*b->len == a->x_off && b->y_off + face_dir[dir][1]*b->len == a->y_off) {
               i = merge_run(c, j, i);
               break;
            }
         }
      }
   }
   a = &c->belts[i];

   for (j=0; j < obarr_len(c->belts); ++j) {
      if (j != i) {
         belt_run *b = &c->belts[j];
         if (b->end_dz == 0 && b->turn == 0 && b->dir == dir && b->z_off == a->z_off && b->type == BR_normal) {
            if (a->x_off + face_dir[dir][0]*a->len == b->x_off && a->y_off + face_dir[dir][1]*a->len == b->y_off) {
               i = merge_run(c, i, j);
               break;  
            }
         }
      }
   }

   c->belts[i].target_id = TARGET_unknown;
   compute_mobile_slots(&c->belts[i]);
}

static logi_chunk_coord coord(int x, int y, int z)
{
   logi_chunk_coord lcc;
   lcc.unpacked.x = x;
   lcc.unpacked.y = y;
   lcc.unpacked.z = z;
   return lcc;
}

static void create_picker(int x, int y, int z, int type, int rot)
{
   logi_chunk *c = get_chunk(x,y,z);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   picker_info pi = { 0 };
   pi.pos = coord(ox,oy,oz);
   pi.type = type;
   pi.rot = rot;
   obarr_push(c->pickers, pi, "/logi/pickerlist");
}

static void destroy_picker(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   int i;
   logi_chunk_coord sc = coord(ox,oy,oz);
   for (i=0; i < obarr_len(c->pickers); ++i) {
      if (c->pickers[i].pos.packed == sc.packed) {
         obarr_fastdelete(c->pickers, i);
         break;
      }
   }
}

static int find_ore(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   int i;
   logi_chunk_coord sc = coord(ox,oy,oz);
   for (i=0; i < obarr_len(c->ore); ++i)
      if (c->ore[i].pos.packed == sc.packed)
         return i;
   return -1;
}

static void create_machine(int x, int y, int z, int type, int rot, int bx, int by, int bz)
{
   logi_chunk *c = get_chunk(x,y,z);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   int ore_z;
   logi_chunk *d;
   machine_info mi = { 0 };
   mi.pos  = coord(ox,oy,oz);
   mi.type = type;
   mi.rot = rot;
   obarr_push(c->machine, mi, "/logi/machinelist");

   d = logistics_get_chunk_alloc(bx+ox,by+oy,bz+oz-1);
   ore_z = (oz-1) & (LOGI_CHUNK_SIZE_Z-1);
   // @TODO only do followign test for BT_ore_driller
   if (d->type[ore_z][oy][ox] == BT_stone || d->type[ore_z][oy][ox] == BT_asphalt) {
      int id = find_ore(x,y,z-1);
      if (id < 0) {
         ore_info ore;
         ore.pos = coord(ox,oy,ore_z);
         ore.count = 5;
         ore.type = d->type[ore_z][oy][ox];
         obarr_push(d->ore, ore, "/logi/orelist");
      }
   }
}

static void create_belt_machine(int x, int y, int z, int type, int rot, int bx, int by, int bz)
{
   logi_chunk *c = get_chunk(x,y,z);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   belt_machine_info bmi = { 0 };
   bmi.pos  = coord(ox,oy,oz);
   bmi.type = type;
   bmi.rot = rot;
   obarr_push(c->belt_machine, bmi, "/logi/machinelist");
}

static int get_belt_id_noramp(int x, int y, int z)
{
   int j;

   logi_chunk *c = get_chunk(x,y,z);

   int ox = x & (LOGI_CHUNK_SIZE_X-1);
   int oy = y & (LOGI_CHUNK_SIZE_Y-1);
   int oz = z & (LOGI_CHUNK_SIZE_Z-1);

   if (c != NULL)
      for (j=0; j < obarr_len(c->belts); ++j)
         if (c->belts[j].end_dz == 0 && c->belts[j].turn == 0)
            if (does_belt_intersect(&c->belts[j], ox,oy,oz))
               return j;
   return -1;
}

static int find_machine(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int i;
   logi_chunk_coord sc;
   sc.unpacked.x = (uint8) LOGI_CHUNK_MASK_X(x);
   sc.unpacked.y = (uint8) LOGI_CHUNK_MASK_Y(y);
   sc.unpacked.z = (uint8) LOGI_CHUNK_MASK_Z(z);
   for (i=0; i < obarr_len(c->machine); ++i)
      if (c->machine[i].pos.packed == sc.packed)
         return i;
   return -1;
}

static int find_belt_machine(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   int i;
   logi_chunk_coord sc;
   sc.unpacked.x = (uint8) LOGI_CHUNK_MASK_X(x);
   sc.unpacked.y = (uint8) LOGI_CHUNK_MASK_Y(y);
   sc.unpacked.z = (uint8) LOGI_CHUNK_MASK_Z(z);
   for (i=0; i < obarr_len(c->belt_machine); ++i)
      if (c->belt_machine[i].pos.packed == sc.packed)
         return i;
   return -1;
}

static void destroy_machine(int x, int y, int z)
{
   int n = find_machine(x,y,z);
   if (n >= 0) {
      logi_chunk *c = get_chunk(x,y,z);
      obarr_fastdelete(c->machine, n);
   }
}

static void destroy_belt_machine(int x, int y, int z)
{
   int n = find_belt_machine(x,y,z);
   if (n >= 0) {
      logi_chunk *c = get_chunk(x,y,z);
      obarr_fastdelete(c->belt_machine, n);
   }
}

static int get_machine_id(int x, int y, int z)
{
   return find_machine(x,y,z);
}

// @TODO what happens if we point a conveyor at the side of a turn
// @TODO make all functions returning belt_id return TARGET_none

static int get_belt_id(int x, int y, int z, int dir)
{
   int j;
   logi_chunk *d = get_chunk(x,y,z);
   int base_x = x & ~(LOGI_CHUNK_SIZE_X-1);
   int base_y = y & ~(LOGI_CHUNK_SIZE_Y-1);
   int base_z = z & ~(LOGI_CHUNK_SIZE_Z-1);
   int ex = x-base_x;
   int ey = y-base_y;
   int ez = z-base_z;

   for (j=0; j < obarr_len(d->belts); ++j) {
      // don't feed from conveyor/ramp that points to side of ramp
      // and don't feed to side of turn
      if (d->belts[j].dir == dir || (d->belts[j].turn==0 && d->belts[j].end_dz==0)) {
         if (does_belt_intersect(&d->belts[j], ex,ey,ez)) {
            #if 0
            if (d->belts[j].dir == outdir && b->target_is_neighbor) {
               d->belts[j].input_id = i;
               d->belts[j].input_dz = b->end_dz;
            }
            #endif
            return j;
         }
      }
   }
   return TARGET_none;
}

static void logistics_update_chunk(int x, int y, int z)
{
   logi_chunk *c = get_chunk(x,y,z);
   if (c != NULL) {
      int base_x = x & ~(LOGI_CHUNK_SIZE_X-1);
      int base_y = y & ~(LOGI_CHUNK_SIZE_Y-1);
      int base_z = z & ~(LOGI_CHUNK_SIZE_Z-1);
      int i,j;
      for (i=0; i < obarr_len(c->belts); ++i) {
         if (1) { //c->belts[i].target_id == TARGET_unknown) {
            logi_chunk *d;
            belt_run *b = &c->belts[i];
            int turn = b->type == BR_splitter ? 3 : b->turn;
            int outdir = (b->dir + turn)&3;
            int ex = b->x_off + b->len * face_dir[outdir][0];
            int ey = b->y_off + b->len * face_dir[outdir][1];
            int ez = b->z_off + b->end_dz, is_neighbor=0;
            if (ex < 0 || ey < 0 || ex >= LOGI_CHUNK_SIZE_X || ey >= LOGI_CHUNK_SIZE_Y || ez < 0 || ez >= LOGI_CHUNK_SIZE_Z) {
               d = get_chunk(base_x + ex, base_y + ey, base_z + ez);
               ex = LOGI_CHUNK_MASK_X(ex);
               ey = LOGI_CHUNK_MASK_Y(ey);
               b->target_is_neighbor = 1;
            } else {
               d = c;
               b->target_is_neighbor = 0;
            }

            if (d != NULL) {
               for (j=0; j < obarr_len(d->belts); ++j) {
                  // don't feed from conveyor/ramp that points to side of ramp
                  // and don't feed to side of turn
                  if (d->belts[j].dir == outdir || (d->belts[j].turn==0 && d->belts[j].end_dz==0)) {
                     if (does_belt_intersect(&d->belts[j], ex,ey,ez)) {
                        b->target_id = j;
                        break;
                     }
                  }
               }
               if (j == obarr_len(d->belts)) {
                  b->target_id = TARGET_none;
               }
            } else
               b->target_id = TARGET_none;

            if (b->type == BR_splitter) {
               outdir = (b->dir + 1)&3;
               ex = b->x_off + b->len * face_dir[outdir][0];
               ey = b->y_off + b->len * face_dir[outdir][1];
               ez = b->z_off + b->end_dz, is_neighbor=0;
               if (ex < 0 || ey < 0 || ex >= LOGI_CHUNK_SIZE_X || ey >= LOGI_CHUNK_SIZE_Y || ez < 0 || ez >= LOGI_CHUNK_SIZE_Z) {
                  d = get_chunk(base_x + ex, base_y + ey, base_z + ez);
                  ex = LOGI_CHUNK_MASK_X(ex);
                  ey = LOGI_CHUNK_MASK_Y(ey);
               }

               if (d != NULL) {
                  for (j=0; j < obarr_len(d->belts); ++j) {
                     // don't feed from conveyor/ramp that points to side of ramp
                     // and don't feed to side of turn
                     if (d->belts[j].dir == outdir || (d->belts[j].turn==0 && d->belts[j].end_dz==0)) {
                        if (does_belt_intersect(&d->belts[j], ex,ey,ez)) {
                           b->target2_id = j;
                           break;
                        }
                     }
                  }
                  if (j == obarr_len(d->belts)) {
                     b->target2_id = TARGET_none;
                  }
               } else
                  b->target2_id = TARGET_none;
            }
         }
      }
      {
         int i;
         for (i=0; i < obarr_len(c->belts); ++i)
            assert(c->belts[i].target_id != TARGET_unknown);
      }

      for (i=0; i < obarr_len(c->pickers); ++i) {
         picker_info *p = &c->pickers[i];
         int ix = base_x + p->pos.unpacked.x + face_dir[p->rot  ][0];
         int iy = base_y + p->pos.unpacked.y + face_dir[p->rot  ][1];
         int ox = base_x + p->pos.unpacked.x + face_dir[p->rot^2][0];
         int oy = base_y + p->pos.unpacked.y + face_dir[p->rot^2][1];
         int id;

         id = get_belt_id_noramp(ix,iy, base_z + p->pos.unpacked.z);
         if (id >= 0) {
            p->input_id = id;
            p->input_is_belt = True;
         } else {
            id = get_machine_id(ix, iy, base_z + p->pos.unpacked.z);
            p->input_id = id >= 0 ? id : TARGET_none;
            p->input_is_belt = False;
         }

         id = get_belt_id_noramp(ox, oy, base_z + p->pos.unpacked.z);
         if (id >= 0) {
            p->output_id = id;
            p->output_is_belt = True;
         } else {
            id = get_machine_id(ox, oy, base_z + p->pos.unpacked.z);
            p->output_id = id >= 0 ? id : TARGET_none;
            p->output_is_belt = False;
         } 
      }

      for (i=0; i < obarr_len(c->machine); ++i) {
         machine_info *m = &c->machine[i];
         if (m->type == BT_ore_drill) {
            int ore_z = base_z + m->pos.unpacked.z - 1;
            logi_chunk *d = logistics_get_chunk_alloc(base_x + m->pos.unpacked.x, base_y + m->pos.unpacked.y, ore_z);
            #if 0
            if (d->type[ore_z & (LOGI_CHUNK_SIZE_Z-1)][m->pos.unpacked.y][m->pos.unpacked.x] == BT_stone)
               m->input_flags = 1;
            else
               m->input_flags = 0;
            #endif
         }
      }

      for (i=0; i < obarr_len(c->belt_machine); ++i) {
         belt_machine_info *m = &c->belt_machine[i];
         int belt_z = base_z + m->pos.unpacked.z - 1;
         int x = base_x + m->pos.unpacked.x;
         int y = base_y + m->pos.unpacked.y;
         logi_chunk *d = logistics_get_chunk_alloc(base_x, base_y, belt_z);
         if (d->type[belt_z & (LOGI_CHUNK_SIZE_Z-1)][m->pos.unpacked.y][m->pos.unpacked.x] == BT_conveyor) {
            int id = get_belt_id_noramp(x,y,belt_z);
            if (id >= 0) {
               m->input_id = (uint16) id;
            } else
               m->input_id = TARGET_none;
         } else {
            m->input_id = TARGET_none;
         }
      }
   }
}

#define IS_RAMP_HEAD(x) \
   (    ((x) == BT_conveyor_ramp_up_low    )    \
     || ((x) == BT_conveyor_ramp_down_high )   )

//
//  
//

static void logistics_update_block_core(int x, int y, int z, int type, int rot, Bool alloc)
{
   logi_chunk *c = alloc ? logistics_get_chunk_alloc(x,y,z) : logistics_get_chunk(x,y,z,0);
   int ox = LOGI_CHUNK_MASK_X(x);
   int oy = LOGI_CHUNK_MASK_Y(y);
   int oz = LOGI_CHUNK_MASK_Z(z);
   int oldtype;

   if (c == NULL)
      return;

   oldtype = c->type[oz][oy][ox];

   if (oldtype == BT_conveyor)
      split_belt(x,y,z, c->rot[oz][oy][ox]);

   if (oldtype >= BT_picker && oldtype <= BT_picker)
      destroy_picker(x,y,z);
   else if (oldtype >= BT_belt_machines)
      destroy_belt_machine(x,y,z);
   else if (oldtype >= BT_machines)
      destroy_machine(x,y,z);
   else if (oldtype == BT_conveyor && type != BT_conveyor)
      destroy_belt(x,y,z);

   c->type[oz][oy][ox] = type;
   c->rot[oz][oy][ox] = rot;

   if (type == BT_conveyor)
      create_belt(c, ox,oy,oz, rot);

   if (type >= BT_picker && type <= BT_picker)
      create_picker(x,y,z, type,rot);
   else if (type >= BT_belt_machines)
      create_belt_machine(x,y,z, type, rot, x-ox,y-oy,z-oz);
   else if (type >= BT_machines)
      create_machine(x,y,z, type, rot, x-ox,y-oy,z-oz);

   if (type == BT_conveyor_90_left || type == BT_conveyor_90_right) {
      if (oldtype == BT_conveyor_90_left || oldtype == BT_conveyor_90_right) {
         // @TODO changing turn types
         destroy_ramp_or_turn(x,y,z);
         create_turn(x,y,z, rot, type == BT_conveyor_90_left ? 1 : -1);
      } else {
         create_turn(x,y,z, rot, type == BT_conveyor_90_left ? 1 : -1);
      }
   } else if (oldtype == BT_conveyor_90_left || oldtype == BT_conveyor_90_right) {
      destroy_ramp_or_turn(x,y,z);
   }

   if (oldtype == BT_splitter)
      destroy_ramp_or_turn(x,y,z);
   if (type == BT_splitter)
      create_splitter(x,y,z, rot);

   if (IS_RAMP_HEAD(type)) {
      if (IS_RAMP_HEAD(oldtype)) {
         // @TODO changing ramp types
         destroy_ramp_or_turn(x,y,z);
         create_ramp(x,y,z, rot, type == BT_conveyor_ramp_up_low ? 1 : -1);
      } else {
         create_ramp(x,y,z, rot, type == BT_conveyor_ramp_up_low ? 1 : -1);
      }
   } else if (IS_RAMP_HEAD(oldtype)) {
      destroy_ramp_or_turn(x,y,z);
   }

   logistics_update_chunk(x,y,z);
   logistics_update_chunk(x - LOGI_CHUNK_SIZE_X, y, z);
   logistics_update_chunk(x + LOGI_CHUNK_SIZE_X, y, z);
   logistics_update_chunk(x, y - LOGI_CHUNK_SIZE_Y, z);
   logistics_update_chunk(x, y + LOGI_CHUNK_SIZE_Y, z);
   logistics_update_chunk(x, y, z - LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x, y, z + LOGI_CHUNK_SIZE_Z);

   // why do we have to update the diagonally-adjacent chunks?!?
   logistics_update_chunk(x - LOGI_CHUNK_SIZE_X, y, z - LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x - LOGI_CHUNK_SIZE_X, y, z + LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x + LOGI_CHUNK_SIZE_X, y, z - LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x + LOGI_CHUNK_SIZE_X, y, z + LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x, y - LOGI_CHUNK_SIZE_Y, z - LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x, y - LOGI_CHUNK_SIZE_Y, z + LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x, y + LOGI_CHUNK_SIZE_Y, z - LOGI_CHUNK_SIZE_Z);
   logistics_update_chunk(x, y + LOGI_CHUNK_SIZE_Y, z + LOGI_CHUNK_SIZE_Z);
}

typedef struct
{
   int x, y, z, type, rot;
   Bool alloc;
} logistics_block_update;
static threadsafe_queue block_update_queue;

static void logistics_update_block_queue_process(void)
{
   logistics_block_update bu;
   while (get_from_queue_nonblocking(&block_update_queue, &bu))
      logistics_update_block_core(bu.x, bu.y, bu.z, bu.type, bu.rot, bu.alloc);
}

static void logistics_update_block_core_queue(int x, int y, int z, int type, int rot, Bool alloc)
{
   logistics_block_update bu = { x,y,z,type,rot,alloc };
   if (!add_to_queue(&block_update_queue, &bu)) {
      error("Overflowed block update queue");
   }
}

static uint32 logistics_long_tick;

static int get_interaction_pos(belt_run *b, int x, int y, int z)
{
   x = x & (LOGI_CHUNK_SIZE_X-1);
   y = y & (LOGI_CHUNK_SIZE_Y-1);
   z = z & (LOGI_CHUNK_SIZE_Z-1);
   return abs(x-b->x_off) + abs(y-b->y_off);
}

static vec3i get_coord_in_dir(int x, int y, int z, int dir)
{
   vec3i result;
   result.x = x + face_dir[dir][0];
   result.y = y + face_dir[dir][1];
   result.z = z;
   return result;
}

static vec3i get_target(int x, int y, int z, belt_run *br)
{
   int outdir = (br->dir + br->turn) & 3;
   vec3i result;
   result.x = x + br->len * face_dir[outdir][0];
   result.y = y + br->len * face_dir[outdir][1];
   result.z = z + br->end_dz;
   return result;
}

static belt_run *get_belt_run_in_direction(int x, int y, int z, int dir, int id, int *off)
{
   logi_chunk *c;
   x = x + face_dir[dir][0];
   y = y + face_dir[dir][1];
   c = get_chunk(x,y,z);
   *off = get_interaction_pos(&c->belts[id], x,y,z);
   assert(id != TARGET_none);
   assert(id < obarr_len(c->belts));
   return &c->belts[id];
}

static void check(belt_run *br)
{
   int i;
   for (i=0; i < obarr_len(br->items); ++i)
      assert(br->items[i].type < 4);
}

// case 1:
// e.g. from east to north
//
//                 ^  ^
//  [1] X X X X -> O  O 
//                 O  O
//  [0] X X X X -> O  O
//                 O  O

// case 3:
// e.g. from west to north
//
//   ^  ^
//   O  O <- X X X X [0]
//   O  O
//   O  O <- X X X X [1]
//   O  O
static void add_item_to_belt_pos(belt_run *b, int slot, int side, int pos, int type);

static vec3i get_belt_target(int x, int y, int z, belt_run *br, int side)
{
   if (br->type == BR_normal) {
      return get_target(x,y,z,br);
   } else if (br->type == BR_splitter) {
      if (side == RIGHT)
         return get_coord_in_dir(x,y,z, (br->dir+3)&3);
      else
         return get_coord_in_dir(x,y,z, (br->dir+1)&3);
   } else {
      vec3i v = { 0,0,0 };
      assert(0);
      return v;
   }
}

static void logistics_belt_tick(int x, int y, int z, belt_run *br)
{
   int side;
   for (side = 0; side < 2; ++side) {
      int start, len, end, j;
      int target_id;

      if (br->type == BR_splitter) {
         start = side ? left_offset(br) : 0;
         len = 3;
         end = start+len;
      } else if (br->turn) {
         int right_len = br->turn > 0 ? LONG_SIDE : SHORT_SIDE;
         if (br->turn > 0)
            len = side ? SHORT_SIDE : LONG_SIDE;
         else
            len = side ? LONG_SIDE : SHORT_SIDE;
         start = side ? right_len : 0;
         end = start + len;
      } else {
         start = side ? left_offset(br) : 0;
         len = br->len * ITEMS_PER_BELT_SIDE;
         end = start + len;
      }

      if (global_hack == -1) {
         memset(br->items+start, 0, sizeof(br->items[0]) * len);
         return;
      }

      // at this moment, item[len-1] has just animated all the way off of the belt (if mobile)
      // so, we move item[len-1] out to some other spot

      target_id = br->target_id;
      if (br->type == BR_splitter)
         if (side == LEFT)
            target_id = br->target2_id;

      if (target_id != TARGET_none) {
         int target_side, target_pos, target_start;
         vec3i target = get_belt_target(x,y,z, br, side);
         logi_chunk *tc = get_chunk_v(&target);
         belt_run *tb = &tc->belts[target_id];
         Bool forbid = False;
         int blockdist;
         int outdir = (br->dir + br->turn) & 3;
         vec3i target_belt_coords;
         int target_left_start = tb->turn ? (tb->turn > 0 ? LONG_SIDE : SHORT_SIDE) : left_offset(tb);
         int target_right_start = 0;
         int target_slot;
         int relative_facing = (tb->dir - outdir) & 3;
         static int normal_output[4][2][2] =
         { 
            { { RIGHT,  0,                    }, { LEFT ,  0                    } },
            { { LEFT ,  1,                    }, { LEFT , ITEMS_PER_BELT_SIDE-1 } },
            { {  -1  , -1,                    }, {   -1 , -1                    } },
            { { RIGHT, ITEMS_PER_BELT_SIDE-1, }, { RIGHT,  1                    } },
         };
         static int splitter_output[4][2][2] =
         {
            { { LEFT , ITEMS_PER_BELT_SIDE-2 },   { RIGHT, ITEMS_PER_BELT_SIDE-2 } },
            { {  -1  , -1                    },   { RIGHT,  0                    } },
            { { RIGHT,  1                    },   { LEFT ,  1                    } },
            { { LEFT ,  0                    },   {  -1  , -1                    } },
         };

         if (br->type == BR_normal) {
            target_side =   normal_output[relative_facing][side][0];
            target_pos  =   normal_output[relative_facing][side][1];
         } else if (br->type == BR_splitter) {
            target_side = splitter_output[relative_facing][side][0];
            target_pos  = splitter_output[relative_facing][side][1];
         } else {
            assert(0);
         }

         if (target_side < 0)
            forbid = True;
         if (tb->type == BR_splitter && relative_facing != 0)
            forbid = True;
         if (tb->turn != 0 && relative_facing != 0)
            forbid = True;

         if (!forbid) {
            target_belt_coords.x = (target.x & ~(LOGI_CHUNK_SIZE_X-1)) + tb->x_off;
            target_belt_coords.y = (target.y & ~(LOGI_CHUNK_SIZE_Y-1)) + tb->y_off;
            target_belt_coords.z = (target.z & ~(LOGI_CHUNK_SIZE_Z-1)) + tb->z_off;
            blockdist = abs(target.x - target_belt_coords.x) + abs(target.y - target_belt_coords.y);
            target_pos += blockdist * ITEMS_PER_BELT_SIDE;
            target_start = (target_side ? target_left_start : target_right_start);

            target_slot = target_start + target_pos;

            assert(target_slot < obarr_len(tb->items));
            if (br->items[end-1].type != 0) {
               if (tb->items[target_slot].type == 0) {
                  add_item_to_belt_pos(tb, target_slot, target_side, target_pos, br->items[end-1].type);
                  br->items[end-1].type = 0;
               }
            }
         }
      }

      // at this moment, item[len-2] has just animated all the way to the farthest
      // position that's still on the belt, i.e. it becomes item[len-1]
      for (j=end-2; j >= start; --j)
         if (br->items[j].type != 0 && br->items[j+1].type == 0) {
            br->items[j+1] = br->items[j];
            br->items[j].type = 0;
         }

      if (global_hack && (stb_rand() % 10 < 2))
         if (br->items[start].type == 0 && stb_rand() % 9 < 2)
            br->items[start].type = (uint8) (stb_rand() % 4);
   }
}

static void logistics_belt_compute_mobility(int x, int y, int z, belt_run *br)
{
   int side;
   for (side = 0; side < 2; ++side) {
      int start, len, end, allow_new_frontmost_to_move=0, j;
      int target_id;

      if (br->type == BR_splitter) {
         start = side ? left_offset(br) : 0;
         len = 3;
         end = start+len;
      } else if (br->turn) {
         int right_len = br->turn > 0 ? LONG_SIDE : SHORT_SIDE;
         if (br->turn > 0)
            len = side ? SHORT_SIDE : LONG_SIDE;
         else
            len = side ? LONG_SIDE : SHORT_SIDE;
         start = side ? right_len : 0;
         end = start + len;
      } else {
         start = side ? left_offset(br) : 0;
         len = br->len * ITEMS_PER_BELT_SIDE;
         end = start + len;
      }

      target_id = br->target_id;
      if (br->type == BR_splitter)
         if (side == LEFT)
            target_id = br->target2_id;

      if (target_id != TARGET_none) {
         int target_side, target_pos, target_start;
         vec3i target = get_belt_target(x,y,z, br, side);
         logi_chunk *tc = get_chunk_v(&target);
         belt_run *tb = &tc->belts[target_id];
         Bool forbid = False;
         int blockdist;
         int outdir = (br->dir + br->turn) & 3;
         vec3i target_belt_coords;
         int target_left_start = tb->turn ? (tb->turn > 0 ? LONG_SIDE : SHORT_SIDE) : left_offset(tb);
         int target_right_start = 0;
         int target_slot;
         int relative_facing = (tb->dir - outdir) & 3;
         static int normal_output[4][2][2] =
         { 
            { { RIGHT,  0,                    }, { LEFT ,  0                    } },
            { { LEFT ,  1,                    }, { LEFT , ITEMS_PER_BELT_SIDE-1 } },
            { {  -1  , -1,                    }, {   -1 , -1                    } },
            { { RIGHT, ITEMS_PER_BELT_SIDE-1, }, { RIGHT,  1                    } },
         };
         static int splitter_output[4][2][2] =
         {
            { { LEFT , ITEMS_PER_BELT_SIDE-2 },   { RIGHT, ITEMS_PER_BELT_SIDE-2 } },
            { {  -1  , -1                    },   { RIGHT,  0                    } },
            { { RIGHT,  1                    },   { LEFT ,  1                    } },
            { { LEFT ,  0                    },   {  -1  , -1                    } },
         };

         if (br->type == BR_normal) {
            target_side =   normal_output[relative_facing][side][0];
            target_pos  =   normal_output[relative_facing][side][1];
         } else if (br->type == BR_splitter) {
            target_side = splitter_output[relative_facing][side][0];
            target_pos  = splitter_output[relative_facing][side][1];
         } else {
            assert(0);
         }

         if (target_side < 0)
            forbid = True;
         if (tb->type == BR_splitter && relative_facing != 0)
            forbid = True;
         if (tb->turn != 0 && relative_facing != 0)
            forbid = True;

         if (!forbid) {
            target_belt_coords.x = (target.x & ~(LOGI_CHUNK_SIZE_X-1)) + tb->x_off;
            target_belt_coords.y = (target.y & ~(LOGI_CHUNK_SIZE_Y-1)) + tb->y_off;
            target_belt_coords.z = (target.z & ~(LOGI_CHUNK_SIZE_Z-1)) + tb->z_off;
            blockdist = abs(target.x - target_belt_coords.x) + abs(target.y - target_belt_coords.y);
            target_pos += blockdist * ITEMS_PER_BELT_SIDE;
            target_start = (target_side ? target_left_start : target_right_start);
            target_slot = target_start + target_pos;

            if (target_pos == 0) {
               if (IS_ITEM_MOBILE(tb,target_pos,target_side) || tb->items[target_slot].type == 0)
                  allow_new_frontmost_to_move = True;
            } else {
               // frontmost object goes to 'target_pos', so check if 'target_pos' will be open next
               // time, by checking if 'target_pos-1' is open now, and that 'slot-1' can move
               if (IS_ITEM_MOBILE(tb,target_pos-1,target_side)) {
                  if (tb->items[target_slot-1].type == 0)
                     allow_new_frontmost_to_move = True;
               } else {
                  assert(!IS_ITEM_MOBILE(tb,target_pos,target_side));
                  if (tb->items[target_slot  ].type == 0)
                     allow_new_frontmost_to_move = True;
               }
            }
         }
      }

      // determine which slots are animating
      if (allow_new_frontmost_to_move)
         br->mobile_slots[side] = len+1;
      else {
         for (j=len-1; j >= 0; --j)
            if (br->items[start+j].type == 0)
               break;
         br->mobile_slots[side] = j+1;
      }
   }
}

typedef struct
{
   logi_slice *slice;
   uint16 belt_id;
   uint8 cid;
} belt_ref;

typedef struct
{
   logi_slice *s;
   int cid;
} vtarget_chunk;

static void vget_target_chunk(vtarget_chunk *tc, int x, int y, int z, belt_run *br)
{
   int bx = (x & ~(LOGI_CHUNK_SIZE_X-1));
   int by = (y & ~(LOGI_CHUNK_SIZE_Y-1));
   int bz = (z & ~(LOGI_CHUNK_SIZE_Z-1));
   vec3i target = get_target(x,y,z, br);
   int ex = target.x;
   int ey = target.y;
   int ez = target.z;
   logistics_get_chunk(target.x,target.y,target.z, &tc->s);
   tc->cid = ez >> LOGI_CHUNK_SIZE_Z_LOG2;
}

static void vget_dir_chunk(vtarget_chunk *tc, int x, int y, int z, int dir)
{
   int ex = x + face_dir[dir][0];
   int ey = y + face_dir[dir][1];
   int ez = z;
   logistics_get_chunk(ex,ey,ez, &tc->s);
   tc->cid = ez >> LOGI_CHUNK_SIZE_Z_LOG2;
}

static belt_ref *sorted_ref;
static void visit(belt_ref *ref)
{
   logi_chunk *c = ref->slice->chunk[ref->cid];
   belt_run *br = &c->belts[ref->belt_id];
   if (br->mark == M_temporary) return;
   if (br->mark == M_unmarked) {
      br->mark = M_temporary;
      if (br->type == BR_normal) {
         if (br->target_id != TARGET_none) {
            vtarget_chunk tc;
            belt_ref target;
            vec3i beltloc;
            beltloc.x = ref->slice->slice_x * LOGI_CHUNK_SIZE_X+br->x_off;
            beltloc.y = ref->slice->slice_y * LOGI_CHUNK_SIZE_Y+br->y_off;
            beltloc.z = ref->cid * LOGI_CHUNK_SIZE_Z+br->z_off;
            vget_target_chunk(&tc, beltloc.x, beltloc.y, beltloc.z, br);
            target.belt_id = br->target_id;
            target.cid = tc.cid;
            target.slice = tc.s;
            if (target.belt_id != TARGET_none) assert(target.belt_id < obarr_len(target.slice->chunk[target.cid]->belts));
            visit(&target);
         }
      } else {
         vtarget_chunk tc;
         belt_ref target;
         vec3i beltloc;
         beltloc.x = ref->slice->slice_x * LOGI_CHUNK_SIZE_X+br->x_off;
         beltloc.y = ref->slice->slice_y * LOGI_CHUNK_SIZE_Y+br->y_off;
         beltloc.z = ref->cid * LOGI_CHUNK_SIZE_Z+br->z_off;
         vget_dir_chunk(&tc, beltloc.x, beltloc.y, beltloc.z, (br->dir+3)&3);
         if (br->target_id != TARGET_none) {
            target.belt_id = br->target_id;
            target.cid = tc.cid;
            target.slice = tc.s;
            if (target.belt_id != TARGET_none) assert(target.belt_id < obarr_len(target.slice->chunk[target.cid]->belts));
            visit(&target);
         }
         if (br->target2_id != TARGET_none) {
            vget_dir_chunk(&tc, beltloc.x, beltloc.y, beltloc.z, (br->dir+1)&3);
            target.belt_id = br->target2_id;
            target.cid = tc.cid;
            target.slice = tc.s;
            if (target.belt_id != TARGET_none) assert(target.belt_id < obarr_len(target.slice->chunk[target.cid]->belts));
            visit(&target);
         }
      }

      if (br->mark == M_temporary) {
         br->mark = M_permanent;
         obarr_push(sorted_ref, *ref, "/logi/tick/sorted_ref");
      }
   }
}

static int offset_for_slot(belt_run *b, int slot)
{
   int left_off = left_offset(b);
   if (slot < left_off)
      return 0;
   else
      return left_off;
}

static void remove_item_from_belt(belt_run *b, int slot)
{
   int side,pos, offset;
   assert(slot < obarr_len(b->items));
   b->items[slot].type = 0;
   offset = offset_for_slot(b, slot);
   side = (offset!=0);
   pos = slot - offset;
}

static Bool try_remove_item_from_belt(belt_run *b, int slot, int type1, int type2)
{
   assert(slot < obarr_len(b->items));
   if (b->items[slot].type == type1 || b->items[slot].type == type2) {
      remove_item_from_belt(b, slot);
      return True;
   } else
      return False;
}

static void add_item_to_belt(belt_run *b, int slot, int type)
{
   assert(slot < obarr_len(b->items));
   assert(b->items[slot].type == 0);
   b->items[slot].type = type;
}

static void add_item_to_belt_pos(belt_run *b, int slot, int side, int pos, int type)
{
   assert(slot < obarr_len(b->items));
   assert(b->items[slot].type == 0);
   b->items[slot].type = type;
}

static Bool try_add_item_to_belt(belt_run *b, int slot, int type)
{
   assert(slot < obarr_len(b->items));
   if (b->items[slot].type == 0) {
      add_item_to_belt(b, slot, type);
      return True;
   } else
      return False;
}

static belt_run *find_belt_slot_for_picker(int belt_id, int ix, int iy, int iz, int rot, int *slot)
{
   logi_chunk *d = logistics_get_chunk(ix, iy, iz, 0);
   belt_run *b = &d->belts[belt_id];
   int pos, side, dist;
   assert(belt_id < obarr_len(d->belts));
   pos = get_interaction_pos(b, ix,iy,iz);
   side = ((b->dir - rot)&3) == 1;
   dist = (b->dir == rot) ? 0 : ((b->dir^2) == rot) ? ITEMS_PER_BELT_SIDE-1 : 1;
   *slot = offset_for_side(b, side) + pos * ITEMS_PER_BELT_SIDE + dist;
   assert(*slot < obarr_len(b->items));
   return b;
}

static int input_type_table[][4] =
{
   { 0,0,0,0 }, // BT_machines
   { 0,0,0,0 }, // BT_ore_drill
   { 1,2,3,4 }, // BT_ore_eater
   { IT_coal, IT_iron_ore }, // BT_furanace
   { IT_coal, IT_iron_bar }, // BT_iron_gear_maker
   { IT_iron_bar, IT_iron_gear },
};

static int input_count_table[][4] =
{
   { 0,0,0,0 }, // BT_machines
   { 0,0,0,0 }, // BT_ore_drill
   { 0,0,0,0 }, // BT_ore_eater
   { 1,1,0,0 }, // BT_furanace
   { 1,1,0,0 }, // BT_iron_gear_maker
   { 1,2,0,0 }, // BT_belt_maker

};

static int output_types[][4] =
{
   { 0 },
   { 0 },
   { 0 },
   { IT_iron_bar },
   { IT_iron_gear },
   { IT_conveyor_belt },
};

static void compute_free_input(Bool slot_free[4], machine_info *mi)
{
   //   2 2 1 1
   int slot_used[4], i;
   slot_used[0] = ((mi->input_flags     ) & 3);
   slot_used[1] = ((mi->input_flags >> 2) & 3);
   slot_used[2] = ((mi->input_flags >> 4) & 1);
   slot_used[3] = ((mi->input_flags >> 5) & 1);

   for (i=0; i < 4; ++i)
      slot_free[i] = (slot_used[i] < input_count_table[mi->type-BT_machines][i]);
}

static void add_one_input(machine_info *mi, int i)
{
   if (i == 0) mi->input_flags += 1;
   if (i == 1) mi->input_flags += 1 << 2;
   if (i == 2) mi->input_flags += 1 << 4;
   if (i == 3) mi->input_flags += 1 << 5;
}

static Bool allow_picker_for_machine(picker_info *pi, machine_info *mi, int item)
{
   int i;
   int *input_types = input_type_table[mi->type - BT_machines];
   Bool input_slot_free[MAX_UNIQUE_INPUTS];

   if (mi->type == BT_ore_eater)
      return mi->output == 0;

   compute_free_input(input_slot_free, mi);

   for (i=0; i < MAX_UNIQUE_INPUTS; ++i)
      if (item == input_types[i] && input_slot_free[i])
         return True;
   return False;
}

static void move_from_picker_to_machine(picker_info *pi, machine_info *mi)
{
   int i;
   int *input_types = input_type_table[mi->type - BT_machines];
   Bool input_slot_free[MAX_UNIQUE_INPUTS];

   if (mi->type == BT_ore_eater) {
      mi->output = pi->item;
      pi->item = 0;
      pi->state = 1;
      return;
   }

   compute_free_input(input_slot_free, mi);

   for (i=0; i < MAX_UNIQUE_INPUTS; ++i) {
      if (pi->item == input_types[i] && input_slot_free[i]) {
         assert(pi->item);
         add_one_input(mi, i);
         pi->item = 0;
         pi->state = 1;
         break;
      }
   }
}

static void logistics_longtick_chunk_machines(logi_chunk *c, int base_x, int base_y, int base_z)
{
   int m;
   int i;

   for (i=0; i < obarr_len(c->belt_machine); ++i) {
      belt_machine_info *m = &c->belt_machine[i];
      if (m->input_id != TARGET_none) {
         logi_chunk *d = logistics_get_chunk(base_x, base_y, base_z + m->pos.unpacked.z - 1, 0);
         belt_run *br = &d->belts[m->input_id];
         int pos = get_interaction_pos(br, m->pos.unpacked.x, m->pos.unpacked.y, (m->pos.unpacked.z-1)&(LOGI_CHUNK_SIZE_Z-1));
         switch (m->type) {
            case BT_balancer: {
               int left_off = left_offset(br);
               beltside_item i_left  = br->items[0        + pos*ITEMS_PER_BELT_SIDE+1];
               beltside_item i_right = br->items[left_off + pos*ITEMS_PER_BELT_SIDE+1];
               if (i_left.type != 0 && i_right.type != 0) {
                  // if both sides are occupied, let them pass as-is
               } else if (i_left.type != 0 || i_right.type != 0) {
                  uint8 cur_side = i_left.type != 0 ? 0 : 1;
                  uint8 side = m->state;
                  int right_slot =   0      + pos*ITEMS_PER_BELT_SIDE+1;
                  int left_slot  = left_off + pos*ITEMS_PER_BELT_SIDE+1;
                  if (side != cur_side) {
                     beltside_item temp = br->items[left_slot];
                     br->items[left_slot] = br->items[right_slot];
                     br->items[right_slot] = temp;
                  }
                  m->state = !m->state;
               }
               break;
            }
         }
      }
   }

   for (m=0; m < obarr_len(c->machine); ++m) {
      machine_info *mi = &c->machine[m];
      Bool went_to_zero = False;
      if (mi->timer) {
         --mi->timer;
         went_to_zero = (mi->timer == 0);
      }
      switch (mi->type) {
         case BT_furnace:
            if (went_to_zero)
               mi->output = IT_iron_bar;
            if (mi->timer == 0 && (mi->input_flags == (1|4)) && mi->output==0) {
               mi->input_flags = 0;
               mi->timer = 7;
            }
            break;

         case BT_iron_gear_maker:
            if (went_to_zero)
               mi->output = IT_iron_gear;
            if (mi->timer == 0 && (mi->input_flags == (1|4)) && mi->output==0) {
               mi->input_flags = 0;
               mi->timer = 11;
            }
            break;

         case BT_conveyor_belt_maker:
            if (went_to_zero)
               mi->output = IT_conveyor_belt;
            if (mi->timer == 0 && (mi->input_flags == (1|8)) && mi->output==0) {
               mi->input_flags = 0;
               mi->timer = 14;
            }
            break;

         case BT_ore_eater:
            if (went_to_zero)
               mi->output = 0;
            if (mi->output != 0 && mi->timer == 0)
               mi->timer = 13;
            break;

         case BT_ore_drill:
            if (went_to_zero) {
               int ore_id = find_ore(base_x+mi->pos.unpacked.x, base_y+mi->pos.unpacked.y, base_z+mi->pos.unpacked.z-1);
               if (ore_id >= 0) {
                  ore_info *oi = &c->ore[ore_id];
                  assert(mi->output == 0);
                  switch (oi->type) {
                     case BT_stone  : mi->output = IT_iron_ore; break;
                     case BT_asphalt: mi->output = IT_coal    ; break;
                  }
               }
            }
            if (mi->timer == 0 && mi->output == 0) {
               int ore_id = find_ore(base_x+mi->pos.unpacked.x, base_y+mi->pos.unpacked.y, base_z+mi->pos.unpacked.z-1);
               if (ore_id >= 0) {
                  ore_info *oi = &c->ore[ore_id];
                  if (oi->count)
                     mi->timer = 7; // start drilling
               }
            }
            break;
      }
   }

   for (m=0; m < obarr_len(c->pickers); ++m) {
      picker_info *pi = &c->pickers[m];
      Bool went_to_zero = False;
      if (pi->state == 1)
         pi->state = 0;
      if (pi->state == 3)
         pi->state = 2;

      if (pi->state == 0) {
         if (pi->item == 0 && pi->input_id != TARGET_none && pi->output_id != TARGET_none && pi->state == 0) {
            int ix = base_x + pi->pos.unpacked.x + face_dir[pi->rot  ][0];
            int iy = base_y + pi->pos.unpacked.y + face_dir[pi->rot  ][1];
            int iz = base_z + pi->pos.unpacked.z;
            if (pi->input_is_belt) {
               int slot;
               belt_run *b = find_belt_slot_for_picker(pi->input_id, ix,iy,iz, pi->rot, &slot);
               if (b->items[slot].type != 0) {
                  if (!pi->output_is_belt) {
                     int ox = base_x + pi->pos.unpacked.x + face_dir[pi->rot^2][0];
                     int oy = base_y + pi->pos.unpacked.y + face_dir[pi->rot^2][1];
                     int oz = base_z + pi->pos.unpacked.z;
                     logi_chunk *d = logistics_get_chunk(ox, oy, oz, 0);
                     machine_info *mi;
                     assert(pi->output_id < obarr_len(d->machine));
                     mi = &d->machine[pi->output_id];
                     if (!allow_picker_for_machine(pi, mi, b->items[slot].type))
                        goto skip_picker;
                     pi->item = b->items[slot].type;
                     remove_item_from_belt(b, slot);
                     pi->state = 3;
                  }
               }
            } else {
               // @TODO forbid picking up a thing that our output doesn't allow
               logi_chunk *d = logistics_get_chunk(ix, iy, base_z + pi->pos.unpacked.z, 0);
               machine_info *mi;
               assert(pi->input_id < obarr_len(d->machine));
               mi = &d->machine[pi->input_id];
               if (mi->output != 0) {
                  pi->item = mi->output;
                  mi->output = 0;
                  pi->state = 3;
               }
            }
         }
        skip_picker:
         ;
      } else if (pi->state == 2) {
         assert(pi->item != 0);
         if (pi->output_id != TARGET_none) {
            int ox = base_x + pi->pos.unpacked.x + face_dir[pi->rot^2][0];
            int oy = base_y + pi->pos.unpacked.y + face_dir[pi->rot^2][1];
            int oz = base_z + pi->pos.unpacked.z;
            assert(pi->item != 0);
            if (pi->output_is_belt) {
               int slot;
               belt_run *b = find_belt_slot_for_picker(pi->output_id, ox,oy,oz, pi->rot^2, &slot);
               if (b->items[slot].type == 0) {
                  add_item_to_belt(b, slot, pi->item);
                  pi->item = 0;
                  pi->state = 1;
               }
            } else {
               logi_chunk *d = logistics_get_chunk(ox, oy, base_z + pi->pos.unpacked.z, 0);
               machine_info *mi;
               assert(pi->output_id < obarr_len(d->machine));
               mi = &d->machine[pi->output_id];
               move_from_picker_to_machine(pi,mi);
            }
         }
      }
   }
}

extern int selected_block[3];
extern int sort_order_for_selected_belt;

static void logistics_do_long_tick(void)
{
   int i,j,k,m;
   belt_ref *belts = NULL;

   for (j=0; j < LOGI_CACHE_SIZE; ++j) {
      for (i=0; i < LOGI_CACHE_SIZE; ++i) {
         logi_slice *s = &logi_world[j][i];
         if (s->slice_x != i+1) {
            for (k=0; k < stb_arrcount(s->chunk); ++k) {
               logi_chunk *c = s->chunk[k];
               if (c != NULL) {
                  for (m=0; m < obarr_len(c->belts); ++m) {
                     belt_ref br;
                     br.belt_id = m;
                     br.cid = k;
                     br.slice = s;
                     obarr_push(belts, br, "/logi/tick/active_belts");
                     c->belts[m].mark = M_unmarked;
                  }
               }
            }
         }
      }
   }

   for (i=0; i < obarr_len(belts); ++i) {
      #if 0 // @TODO
      belt_ref *b = &belts[i];
      belt_run *br = &b->slice->chunk[b->cid]->belts[b->belt_id];
      assert(b->belt_id < obarr_len(b->slice->chunk[b->cid]->belts));
      #else
      belt_run *br = &belts[i].slice->chunk[belts[i].cid]->belts[belts[i].belt_id];
      assert(belts[i].belt_id < obarr_len(belts[i].slice->chunk[belts[i].cid]->belts));
      #endif
      if (br->mark == M_unmarked) {
         visit(&belts[i]);
      }
   }
   obarr_free(belts);
   belts = NULL;

   sort_order_for_selected_belt = -1; // selected belt
   for (i=0; i < obarr_len(sorted_ref); ++i) {
      belt_ref *r = &sorted_ref[i];
      logi_slice *s = r->slice;
      belt_run *br = &s->chunk[r->cid]->belts[r->belt_id];
      int obarrlen = obarr_len(s->chunk[r->cid]->belts);
      int belt_id = r->belt_id;
      int base_x = s->slice_x * LOGI_CHUNK_SIZE_X;
      int base_y = s->slice_y * LOGI_CHUNK_SIZE_Y;
      int base_z = r->cid     * LOGI_CHUNK_SIZE_Z;
      assert(belt_id < obarrlen);
      logistics_belt_tick(base_x+br->x_off,base_y+br->y_off,base_z+br->z_off, br);
      br->mark = M_unmarked;
      if (does_belt_intersect(br, selected_block[0]-base_x, selected_block[1]-base_y, selected_block[2]-base_z))
         sort_order_for_selected_belt = i;
   }

   for (j=0; j < LOGI_CACHE_SIZE; ++j) {
      for (i=0; i < LOGI_CACHE_SIZE; ++i) {
         logi_slice *s = &logi_world[j][i];
         if (s->slice_x != i+1) {
            int base_x = s->slice_x * LOGI_CHUNK_SIZE_X;
            int base_y = s->slice_y * LOGI_CHUNK_SIZE_Y;
            for (k=0; k < stb_arrcount(s->chunk); ++k) {
               int base_z = k * LOGI_CHUNK_SIZE_Z;
               logi_chunk *c = s->chunk[k];
               if (c != NULL) {
                  logistics_longtick_chunk_machines(c, base_x, base_y, base_z);
               }
            }
         }
      }
   }

   for (i=0; i < obarr_len(sorted_ref); ++i) {
      belt_ref *r = &sorted_ref[i];
      logi_slice *s = r->slice;
      belt_run *br = &s->chunk[r->cid]->belts[r->belt_id];
      int obarrlen = obarr_len(s->chunk[r->cid]->belts);
      int belt_id = r->belt_id;
      int base_x = s->slice_x * LOGI_CHUNK_SIZE_X;
      int base_y = s->slice_y * LOGI_CHUNK_SIZE_Y;
      int base_z = r->cid     * LOGI_CHUNK_SIZE_Z;
      assert(belt_id < obarrlen);
      logistics_belt_compute_mobility(base_x+br->x_off,base_y+br->y_off,base_z+br->z_off, br);
   }

   obarr_free(sorted_ref);
   sorted_ref = NULL;
}

static void copy_belt(render_belt_run *rbr, belt_run *br)
{
   uint i;
   size_t max_items = ITEMS_PER_BELT*br->len;
   size_t size = sizeof(beltside_item) * max_items;
   rbr->items = obbg_malloc(size, "/thread/logi/copy/belt");

   rbr->pos.unpacked.x = br->x_off;
   rbr->pos.unpacked.y = br->y_off;
   rbr->pos.unpacked.z = br->z_off;
   rbr->dir = br->dir;
   rbr->len = br->len;
   rbr->turn = br->turn;
   rbr->type = br->type;
   rbr->end_dz = br->end_dz;
   for (i=0; i < 2; ++i)
      rbr->mobile_slots[i] = br->mobile_slots[i];

   for (i=0; i < max_items; ++i)
      rbr->items[i] = br->items[i].type;
}

static void copy_belt_machine(render_belt_machine_info *rbm, belt_machine_info *bm)
{
   rbm->pos = bm->pos;
   rbm->type = bm->type;
   rbm->rot = bm->rot;
   rbm->state = bm->state;
   rbm->slots[0] = bm->slots[0];
   rbm->slots[1] = bm->slots[1];
}

static void copy_machine(render_machine_info *rm, machine_info *mi)
{
   rm->pos = mi->pos;
   rm->type = mi->type;
   rm->timer = mi->timer;
   rm->output = mi->output;
   rm->config = mi->config;
   rm->uses = mi->uses;
   rm->input_flags = mi->input_flags;
   rm->rot = mi->rot;
}

static void copy_picker(render_picker_info *rp, picker_info *pi)
{
   rp->pos = pi->pos;
   rp->type = pi->type;
   rp->state = pi->state;
   rp->item = pi->item;
   rp->rot = pi->rot;
   rp->input_is_belt = 1;
   rp->output_is_belt = 1;
}

static render_logi_chunk *copy_logi_chunk(logi_chunk *c, int slice_x, int slice_y, int chunk_z)
{
   int i;

   render_logi_chunk *rc;

   rc = obbg_malloc(sizeof(*rc), "/thread/logi/copy/chunk");
   rc->slice_x = slice_x;
   rc->slice_y = slice_y;
   rc->chunk_z = chunk_z;

   rc->num_belts         = obarr_len(c->belts);
   rc->num_machines      = obarr_len(c->machine);
   rc->num_belt_machines = obarr_len(c->belt_machine);
   rc->num_pickers       = obarr_len(c->pickers);

   rc->belts        = obbg_malloc(sizeof(rc->belts       [0]) * rc->num_belts        ,"/thread/logi/copy/belt");
   rc->machine      = obbg_malloc(sizeof(rc->machine     [0]) * rc->num_machines     ,"/thread/logi/copy/machine");
   rc->belt_machine = obbg_malloc(sizeof(rc->belt_machine[0]) * rc->num_belt_machines,"/thread/logi/copy/machine");
   rc->pickers      = obbg_malloc(sizeof(rc->pickers     [0]) * rc->num_pickers      ,"/thread/logi/copy/pickers");

   for (i=0; i < rc->num_belts        ; ++i) copy_belt        (&rc->belts       [i], &c->belts       [i]);
   for (i=0; i < rc->num_belt_machines; ++i) copy_belt_machine(&rc->belt_machine[i], &c->belt_machine[i]);
   for (i=0; i < rc->num_pickers      ; ++i) copy_picker      (&rc->pickers     [i], &c->pickers     [i]);
   for (i=0; i < rc->num_machines     ; ++i) copy_machine     (&rc->machine     [i], &c->machine     [i]);

   return rc;
}

static void free_render_logi_chunk(render_logi_chunk *rc)
{
   int i;
   for (i=0; i < rc->num_belts; ++i)
      obbg_free(rc->belts[i].items);
   obbg_free(rc->belts);
   obbg_free(rc->machine);
   obbg_free(rc->pickers);
   obbg_free(rc->belt_machine);
   obbg_free(rc);
}

volatile render_logi_chunk **render_copy;
void copy_logistics_database(void)
{
   int i,j,k;
   render_logi_chunk **rlc = (render_logi_chunk **) render_copy;

   // free old render state, since renderer must be done with it
   for (i=0; i < obarr_len(rlc); ++i)
      free_render_logi_chunk(rlc[i]);
   obarr_free(render_copy);

   for (j=0; j < LOGI_CACHE_SIZE; ++j) {
      for (i=0; i < LOGI_CACHE_SIZE; ++i) {
         logi_slice *s = &logi_world[j][i];
         if (s->slice_x != i+1) {
            for (k=0; k < stb_arrcount(s->chunk); ++k) {
               logi_chunk *c = s->chunk[k];
               if (c != NULL) {
                  render_logi_chunk *rlc = copy_logi_chunk(c, s->slice_x, s->slice_y, k);
                  obarr_push(render_copy, rlc, "foo");
               }
            }
         }
      }
   }
}


static uint32 logistics_ticks;
extern int tex_anim_offset;
extern int hack_ffwd;
static SDL_sem *logistics_copy_start, *logistics_copy_done;
static Bool logistics_on_background_thread=True;

void logistics_tick(void)
{
   while (ore_pending != ore_processed) {
      int i;
      for (i=ore_hack[ore_processed].z1; i < ore_hack[ore_processed].z2; ++i)
         logistics_update_block_core_queue(ore_hack[ore_processed].x, ore_hack[ore_processed].y, i, ore_hack[ore_processed].type, 0, False);

      ++ore_processed;
   }

   logistics_texture_scroll += (1.0f / LONG_TICK_LENGTH / ITEMS_PER_BELT_SIDE) / 4.0f; // texture repeats = 4
   if (logistics_texture_scroll >= 1.0)
      logistics_texture_scroll -= 1.0;

   ++logistics_ticks;
   tex_anim_offset = ((logistics_ticks >> 5) & 3) * 16;

   ++logistics_long_tick;
   if (logistics_long_tick > LONG_TICK_LENGTH)
      logistics_long_tick = LONG_TICK_LENGTH;
}

static vec player_vacuum_loc;
static Bool do_player_vacuum;

void player_vacuum(Bool enable, vec *loc)
{
   do_player_vacuum = enable;
   if (enable)
      player_vacuum_loc = *loc;
}

static int floori(float x)
{
   return (int) floor(x);
}

static int face_orig[4][2] = {
   { 0,0 },
   { 1,0 },
   { 1,1 },
   { 0,1 },
};

Bool axis_overlap(float a_min, float a_max, float b_center, float b_radius)
{
   return a_min <= b_center+b_radius && a_max >= b_center-b_radius;
}

static Bool should_vacuum(float x, float y, float z, vec *pmin, vec *pmax)
{
   float item_radius = 1.0f / ITEMS_PER_BELT_SIDE;
   float player_radius_y = (pmax->y - pmin->y)/2.0f;
   float player_radius_x = (pmax->x - pmin->x)/2.0f;
   float player_radius = (player_radius_x + player_radius_y)/2.0f;
   float threshold_dist = item_radius + player_radius;

   float dx,dy;
   dx = (float) fabs(x - (pmin->x + pmax->x)/2);
   dy = (float) fabs(y - (pmin->y + pmax->y)/2);
   if (!axis_overlap(pmin->z, pmax->z, z, item_radius))
      return False;

   if (dx*dx + dy*dy > threshold_dist * threshold_dist)
      return False;

   return True;
}

static void vacuum_item(belt_run *br, int slot)
{
   assert(br->items[slot].type != 0);
   if (available_inventory_slot(br->items[slot].type)) {
      add_to_inventory(br->items[slot].type);
      br->items[slot].type = 0;
   }
}

// @TODO we don't really want this
#pragma warning(disable:4244)

// @TODO: this is only safe if the main thread is stopped
static void non_logistics_interactions(void)
{
   // do player & non-logistics interactions with logistics contents
   if (do_player_vacuum) {
      float offset=0.5;
      int i,j,k;
      vec pmin, pmax;
      vec3i wmin, wmax;
      vec3i cmin,cmax;
      pmin.x = player_vacuum_loc.x - type_prop[OTYPE_player].hsz_x - 0.25f;
      pmin.y = player_vacuum_loc.y - type_prop[OTYPE_player].hsz_y - 0.25f;
      pmin.z = player_vacuum_loc.z;

      pmax.x = player_vacuum_loc.x + type_prop[OTYPE_player].hsz_x + 0.25f;
      pmax.y = player_vacuum_loc.y + type_prop[OTYPE_player].hsz_y + 0.25f;
      pmax.z = player_vacuum_loc.z + type_prop[OTYPE_player].height + 0.25f;

      wmin.x = floori(pmin.x);
      wmin.y = floori(pmin.y);
      wmin.z = floori(pmin.z);

      wmax.x = floori(pmax.x);
      wmax.y = floori(pmax.y);
      wmax.z = floori(pmax.z);

      cmin.x = wmin.x >> LOGI_CHUNK_SIZE_X_LOG2;
      cmin.y = wmin.y >> LOGI_CHUNK_SIZE_Y_LOG2;
      cmin.z = wmin.z >> LOGI_CHUNK_SIZE_Z_LOG2;
      cmax.x = wmax.x >> LOGI_CHUNK_SIZE_X_LOG2;
      cmax.y = wmax.y >> LOGI_CHUNK_SIZE_Y_LOG2;
      cmax.z = wmax.z >> LOGI_CHUNK_SIZE_Z_LOG2;

      for (j=cmin.y; j <= cmax.y; ++j) {
         for (i=cmin.x; i <= cmax.x; ++i) {
            for (k=cmin.z; k <= cmax.z; ++k) {
               logi_chunk *c = logistics_get_chunk(i*LOGI_CHUNK_SIZE_X, j*LOGI_CHUNK_SIZE_Y, k*LOGI_CHUNK_SIZE_Z, NULL);
               if (c != NULL) {
                  int n,e;
                  vec chunk_base;
                  chunk_base.x = (float) (i << LOGI_CHUNK_SIZE_X_LOG2);
                  chunk_base.y = (float) (j << LOGI_CHUNK_SIZE_Y_LOG2);
                  chunk_base.z = (float) (k << LOGI_CHUNK_SIZE_Z_LOG2);

                  for (n=0; n < obarr_len(c->belts); ++n) {
                     belt_run *b = &c->belts[n];

                     if (b->turn == 0) {
                        float z = chunk_base.z + 1.0f + b->z_off + 0.125f;
                        float x1 = chunk_base.x + b->x_off;
                        float y1 = chunk_base.y + b->y_off;
                        float x2,y2;
                        int len = b->len * ITEMS_PER_BELT_SIDE;
                        int d0 = b->dir;
                        int d1 = (d0 + 1) & 3;

                        x1 += face_orig[b->dir][0];
                        y1 += face_orig[b->dir][1];

                        x1 += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE)*0.5;
                        y1 += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE)*0.5;

                        x2 = x1 + face_dir[d1][0]*(0.9f - 0.5/ITEMS_PER_BELT_SIDE);
                        x1 = x1 + face_dir[d1][0]*(0.1f + 0.5/ITEMS_PER_BELT_SIDE);

                        y2 = y1 + face_dir[d1][1]*(0.9f - 0.5/ITEMS_PER_BELT_SIDE);
                        y1 = y1 + face_dir[d1][1]*(0.1f + 0.5/ITEMS_PER_BELT_SIDE);

                        for (e=0; e < len; ++e) {
                           float ax,ay,az;
                           if (b->items[e+0].type != 0) {
                              ax = x1, ay = y1, az=z;
                              if (IS_ITEM_MOBILE(b,e,RIGHT)) {
                                 ax += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE) * offset;
                                 ay += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE) * offset;
                                 az += b->end_dz / 2.0 * (1.0 / ITEMS_PER_BELT_SIDE) * offset;
                              }
                              if (should_vacuum(ax,ay,az, &pmin, &pmax))
                                 vacuum_item(b, e+0);
                           }
                           if (b->items[e+len].type != 0) {
                              ax = x2, ay = y2, az=z;
                              if (IS_ITEM_MOBILE(b,e,LEFT)) {
                                 ax += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE) * offset;
                                 ay += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE) * offset;
                                 az += b->end_dz / 2.0 * (1.0 / ITEMS_PER_BELT_SIDE) * offset;
                              }
                              if (should_vacuum(ax,ay,az, &pmin, &pmax))
                                 vacuum_item(b, e+len);
                           }
                           x1 += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE);
                           y1 += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE);
                           x2 += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE);
                           y2 += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE);
                           z += b->end_dz / 2.0 * (1.0 / ITEMS_PER_BELT_SIDE);
                        }
                     } else {
                        float z = chunk_base.z + 1.0f + b->z_off + 0.125f;
                        float x1 = chunk_base.x + b->x_off;
                        float y1 = chunk_base.y + b->y_off;
                        float x2,y2;
                        float ox,oy;
                        int d0 = b->dir;
                        int d1 = (d0 + 1) & 3;
                        int left_len = b->turn > 0 ? SHORT_SIDE : LONG_SIDE;
                        int right_len = SHORT_SIDE+LONG_SIDE - left_len;

                        x1 += face_orig[b->dir][0];
                        y1 += face_orig[b->dir][1];

                        ox = x1;
                        oy = y1;
                        if (b->turn > 0) {
                           ox += face_dir[(b->dir+1)&3][0];
                           oy += face_dir[(b->dir+1)&3][1];
                        }

                        x1 += face_dir[d0][0]*(1.0/ITEMS_PER_BELT_SIDE)*0.5;
                        y1 += face_dir[d0][1]*(1.0/ITEMS_PER_BELT_SIDE)*0.5;

                        x2 = x1 + face_dir[d1][0]*(0.9f - 0.5/ITEMS_PER_BELT_SIDE);
                        y2 = y1 + face_dir[d1][1]*(0.9f - 0.5/ITEMS_PER_BELT_SIDE);

                        x1 = x1 + face_dir[d1][0]*(0.1f + 0.5/ITEMS_PER_BELT_SIDE);
                        y1 = y1 + face_dir[d1][1]*(0.1f + 0.5/ITEMS_PER_BELT_SIDE);

                        for (e=0; e < right_len; ++e) {
                           if (b->items[0+e].type != 0) {
                              float bx,by,ax,ay;
                              float ang = e, s,c;
                              if (IS_ITEM_MOBILE(b,e,RIGHT))
                                 ang += offset;
                              ang = (ang / right_len) * 3.141592/2;
                              if (b->turn > 0) ang = -ang;
                              ax = x1-ox;
                              ay = y1-oy;
                              s = sin(ang);
                              c = cos(ang);
                              bx = c*ax + s*ay;
                              by = -s*ax + c*ay;
                              if (should_vacuum(ox+bx, oy+by, z, &pmin, &pmax))
                                 vacuum_item(b, 0+e);
                           }
                        }
                        for (e=0; e < left_len; ++e) {
                           if (b->items[right_len+e].type != 0) {
                              float bx,by,ax,ay;
                              float ang = e, s,c;
                              if (IS_ITEM_MOBILE(b,e,LEFT))
                                 ang += offset;
                              ang = (ang / left_len) * 3.141592/2;
                              if (b->turn > 0) ang = -ang;
                              ax = x2-ox;
                              ay = y2-oy;
                              s = sin(ang);
                              c = cos(ang);
                              bx = c*ax + s*ay;
                              by = -s*ax + c*ay;
                              if (should_vacuum(ox+bx, oy+by, z, &pmin, &pmax))
                                 vacuum_item(b, right_len+e);
                           }
                        }
                     }
                  }
               }
            }
         }
      }
   }
}

void logistics_render(void)
{
   float offset;
   if (logistics_long_tick == LONG_TICK_LENGTH || hack_ffwd) {
      logistics_long_tick = 0;
      if (logistics_on_background_thread) {
         SDL_SemPost(logistics_copy_start);
         SDL_SemWait(logistics_copy_done);
      } else {
         non_logistics_interactions();
         logistics_do_long_tick();
         logistics_update_block_queue_process();
         copy_logistics_database();
      }
   }
   offset = (float) logistics_long_tick / LONG_TICK_LENGTH;// + stb_frand();
   logistics_render_from_copy((render_logi_chunk **) render_copy, offset);
}

// Order of operations:
//    3. Belt & machine contents updates from non-logistics sources
//    1. Copy
//    2. Tick
//    4. Block updates
static int logistics_thread_main(void *data)
{
   data=0;
   for(;;) {
      SDL_SemWait(logistics_copy_start);
      non_logistics_interactions();
      copy_logistics_database();
      SDL_SemPost(logistics_copy_done);

      // tick!
      logistics_do_long_tick();

      // process block update queue
      logistics_update_block_queue_process();
   }
   return 0;
};

void logistics_init(void)
{
   int i,j;
   for (j=0; j < LOGI_CACHE_SIZE; ++j)
      for (i=0; i < LOGI_CACHE_SIZE; ++i)
         logi_world[j][i].slice_x = i+1;
   init_threadsafe_queue(&block_update_queue, 16384, sizeof(logistics_block_update));
   if (logistics_on_background_thread) {
      logistics_copy_start = SDL_CreateSemaphore(0);
      logistics_copy_done  = SDL_CreateSemaphore(0);
      SDL_CreateThread(logistics_thread_main, "logistics thread", (void *) 0);
   }
}

void logistics_record_ore(int x, int y, int z1, int z2, int type)
{
   ore_hack_info *ohi;
   SDL_LockMutex(ore_update_mutex);
   if (uidict == NULL) uidict = stb_uidict_create();

   if (ore_count++ <= 2000) {
      ohi = stb_uidict_get(uidict, y*65536+x);
      if (ohi == NULL) {
         ++ore_pending;
         ohi = next_ohi++;//malloc(sizeof(*ohi));
         ohi->x = x;
         ohi->y = y;
         ohi->z1 = z1;
         ohi->z2 = z2;
         ohi->type = type;
         ohi->padding = 0;
         stb_uidict_add(uidict, y*65536+x, ohi);
      }
   }

   SDL_UnlockMutex(ore_update_mutex);
}

void logistics_update_block(int x, int y, int z, int type, int rot)
{
   if (type == BT_conveyor_ramp_up_low) {
      // place ramp up one lower in logistics world than in physics world, so
      // other belts feed onto it automagically
      logistics_update_block_core_queue(x,y,z,type,rot, True);
      // put a placeholder where the ramp is in the physics world
//      logistics_update_block_core_queue(x,y,z, BT_down_marker, 0, True);
   } else
      logistics_update_block_core_queue(x,y,z,type,rot, True);
}
