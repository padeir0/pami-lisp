#include "pami-lisp.h"
#include <strings.h> /*TODO: reimplement bzero and remove this dependency*/
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * -------------------------------------
 * |    ###GENERAL DEFINITIONS###      |
 * -------------------------------------
 */

#define WORD sizeof(void*)

size_t distance(const uint8_t* a, const uint8_t* b) {
  if (a > b) {
    return a-b;
  } else {
    return b-a;
  }
}

/*
 * -------------------------------------
 * |            ###UTF8###             |
 * -------------------------------------
 */

#define LOW_BITS(n) (uint8_t)((1<<n)-1)
#define TOP_BITS(n) (uint8_t)(~((1<<(8-n))-1))

typedef int32_t rune;

const rune EoF = 0;

size_t utf8_decode(const char* buffer, rune* r) {
  if ((buffer[0] & TOP_BITS(1)) == 0) { /* ASCII */
    *r = (rune)buffer[0];
    return 1;
  } else if ((buffer[0] & TOP_BITS(3)) == TOP_BITS(2)) { /* TWO BYTE SEQUENCE */
    if ((buffer[1] & TOP_BITS(2)) != TOP_BITS(1)) {
      *r = -1;
      return 0;
    }
    *r = (rune)(buffer[0] & LOW_BITS(5)) << 6 |
         (rune)(buffer[1] & LOW_BITS(6));
    return 2;
  } else if ((buffer[0] & TOP_BITS(4)) == TOP_BITS(3)) { /* THREE BYTE SEQUENCE */
    if ((buffer[1] & TOP_BITS(2)) != TOP_BITS(1) ||
        (buffer[2] & TOP_BITS(2)) != TOP_BITS(1)) {
      *r = -1;
      return 0;
    }
    *r = (rune)(buffer[0] & LOW_BITS(4)) << 12 |
         (rune)(buffer[1] & LOW_BITS(6)) << 6 |
         (rune)(buffer[2] & LOW_BITS(6));
    return 3;
  } else if ((buffer[0] & TOP_BITS(5)) == TOP_BITS(4)) { /* FOUR BYTE SEQUENCE */
    if ((buffer[1] & TOP_BITS(2)) != TOP_BITS(1) ||
        (buffer[2] & TOP_BITS(2)) != TOP_BITS(1) ||
        (buffer[3] & TOP_BITS(2)) != TOP_BITS(1)) {
      *r = -1;
      return 0;
    }
    *r = (rune)(buffer[0] & LOW_BITS(3)) << 18 |
         (rune)(buffer[1] & LOW_BITS(6)) << 12 |
         (rune)(buffer[2] & LOW_BITS(6)) << 6 |
         (rune)(buffer[3] & LOW_BITS(6));
    return 3;
  } else { /* INVALID ENCODING */
    *r = -1;
    return 0;
  }
}

/*
 * -------------------------------------
 * |            ###POOL###             |
 * -------------------------------------
 */

enum pool_RES {
  pool_OK,
  /* indicates the pointer to be freed is not within the pool */
  pool_ERR_BOUNDS,
  /* indicates the pointer to be freed is unaligned with chunk boundaries */
  pool_ERR_ALIGN,
  /* indicates given chunk size is too small (minimum is sizeof(node)) */
  pool_ERR_CHUNK_SIZE, 
  /* indicates given buffer is too small (minimum is sizeof(pool) + chunksize) */
  pool_ERR_SMALL_BUFF,
  /* indicates the given buffer is null */
  pool_ERR_NULL_BUFF
};

char* pool_str_res(enum pool_RES r);

typedef struct _pool_snode {
  struct _pool_snode* next;
} pool_node;

typedef struct {
  pool_node* head;
  pool_node* tail;
  uint8_t* begin;
  uint8_t* end;
  size_t chunksize;
  size_t size;
} pool;

/* returns a pool allocated at the beginning of the buffer
 * if the pool is NULL, then error contains the reason.
 */
pool* pool_create(uint8_t* buff, size_t buff_size, size_t chunksize, enum pool_RES* error);

/* tries to allocate a object of size 'chunksize',
 * returns NULL if it fails to allocate
 */
void* pool_alloc(pool* p);

/* frees an object allocated in the pool,
 * returns error if the pointer is incorrectly aligned
 */
enum pool_RES pool_free(pool* p, void* obj);

/* frees all objects in the pool
 */
void pool_free_all(pool* p);

/* returns the amount of memory available
 */
size_t pool_available(const pool* p);

/* returns the amount of memory used
 */
size_t pool_used(const pool* p);

/* returns if the pool is empty
 */
bool pool_empty(const pool* p);

#define POOL_OFFSETNODE(a, x) (pool_node*)((uint8_t*)a + x)

char* pool_str_res(enum pool_RES r) {
  switch (r) {
    case pool_OK:
      return "OK";
    case pool_ERR_BOUNDS:
      return "Pointer is out of bounds";
    case pool_ERR_ALIGN:
      return "Pointer is out of alignment";
    case pool_ERR_CHUNK_SIZE:
      return "Provided chunk size is too small";
    case pool_ERR_SMALL_BUFF:
      return "Provided buffer is too small";
    case pool_ERR_NULL_BUFF:
      return "Buffer is NULL";
  }
  return "??";
}

void pool_set_list(pool* p) {
  pool_node* curr = (pool_node*)p->begin;
  /* we need this because of alignment, the chunks may not align
   * and leave a padding at the end of the buffer
   */
  pool_node* end = POOL_OFFSETNODE(p->end, -p->chunksize);

  p->head = curr;
  while (curr < end) {
    curr->next = POOL_OFFSETNODE(curr, p->chunksize);
    curr = curr->next;
  }

  /* curr is at the edge of the buffer, and may not be valid
   * in case the end is not aligned, we leave padding
   */
  if ((uint8_t*)curr + p->chunksize != p->end) {
    p->end = (uint8_t*)curr;
    curr = POOL_OFFSETNODE(curr, -p->chunksize);
    p->size = distance(p->begin, p->end);
  }

  curr->next = NULL;
  p->tail = curr;
}

const size_t pool_min_chunk_size = sizeof(pool_node);

pool* pool_create(uint8_t* buff, size_t buffsize, size_t chunksize, enum pool_RES* out) {
  pool* p;

  if (buff == NULL) {
    *out = pool_ERR_NULL_BUFF;
    return NULL;
  }

  if (chunksize < pool_min_chunk_size) {
    *out = pool_ERR_CHUNK_SIZE;
    return NULL;
  }

  if (buffsize < sizeof(pool) + chunksize) {
    *out = pool_ERR_SMALL_BUFF;
    return NULL;
  }

  p = (pool*)buff;
  p->begin = buff + sizeof(pool);
  p->end = buff + buffsize;
  p->chunksize = chunksize;
  p->size = distance(p->begin, p->end);

  bzero(p->begin, p->size);
  pool_set_list(p);
  return p;
}

void* pool_alloc(pool* p) {
  void* curr;
  if (p->head == NULL) {
    return NULL;
  }

  curr = p->head;
  p->head = p->head->next;

  if (p->head == NULL) {
    p->tail = NULL;
  }
  return curr;
}

enum pool_RES pool_free(pool* p, void* ptr) {
  pool_node* new;

  if (!(p->begin <= (uint8_t*)ptr && (uint8_t*)ptr < p->end)) {
    return pool_ERR_BOUNDS;
  }

  if (distance(ptr, p->begin) % p->chunksize != 0) {
    return pool_ERR_ALIGN;
  }

  new = (pool_node*)ptr;
  new->next = NULL;

  if (p->head == NULL) {
    p->head = new;
    p->tail = new;
    return pool_OK;
  }

  p->tail->next = new;
  p->tail = new;
  return pool_OK;
}

void pool_free_all(pool* p) {
  pool_set_list(p);
}

size_t pool_available(const pool* p) {
  size_t total = 0;
  pool_node* curr = p->head;
  while (curr != NULL) {
    total += p->chunksize;
    curr = curr->next;
  }
  return total;
}

size_t pool_used(const pool* p) {
  return p->size - pool_available(p);
}

bool pool_empty(const pool* p) {
  size_t total = pool_available(p);
  if (total < p->size) {
    return false;
  }
  return true;
}

/*
 * -------------------------------------
 * |          ###FREELIST###           |
 * -------------------------------------
 */

enum fl_RES {
  fl_OK,
  /* Buffer is too small */
  fl_ERR_SMALLBUFF,
  /* Pointer to be freed is out of bounds */
  fl_ERR_BOUNDS
};

char* fl_str_res(enum fl_RES res);

typedef struct {
  size_t size;
} fl_obj_header;

typedef struct _fl_node {
  size_t size;
  struct _fl_node *next;
} fl_node;

typedef struct {
  fl_node* head;
  uint8_t* begin;
  uint8_t* end;
  size_t   size;
} freelist;

size_t fl_pad(size_t size);
size_t fl_objsize(const void* size);

freelist* fl_create(uint8_t* buffer, size_t size, enum fl_RES* res);

/* tries to allocate a object of the given size,
 * returns NULL if it fails to allocate
 */
void* fl_alloc(freelist* fl, size_t size);

/* frees an object allocated by the freelist
 * if the object was not allocated in that particular freelist,
 * or if the object is uncorrectly aligned,
 * it returns false
 */
enum fl_RES fl_free(freelist* fl, void* obj);

/* frees all objects in the free list */
void fl_free_all(freelist* fl);

/* returns the amount of memory available */
size_t fl_available(const freelist* fl);

/* returns the amount of memory used */
size_t fl_used(const freelist* fl);

/* returns if the heap is empty */
bool fl_empty(const freelist* fl);

#define FL_OFFSETNODE(a, x) (fl_node*)((uint8_t*)a + x)

size_t fl_pad(size_t size) {
  size = size + sizeof(fl_obj_header);
  if (size%WORD != 0) {
    return size + (WORD-size%WORD);
  }
  /* objects need space for a Node when deallocated */
  if (size < sizeof(fl_node)) {
    size = sizeof(fl_node);
  }
  return size;
}

char* fl_str_res(enum fl_RES res) {
  switch (res) {
    case fl_OK:
      return "OK";
    case fl_ERR_SMALLBUFF:
      return "Provided buffer is too small";
    case fl_ERR_BOUNDS:
      return "Pointer is out of bounds";
  }
  return "??";
}

freelist* fl_create(uint8_t* buffer, size_t size, enum fl_RES *res) {
  freelist* fl;
  if (size < sizeof(freelist) + sizeof(fl_node)) {
    *res = fl_ERR_SMALLBUFF;
    return NULL;
  }

  fl = (freelist*)buffer;
  fl->head = (fl_node*)(buffer + sizeof(freelist));
  fl->head->size = size - sizeof(freelist);
  fl->head->next = NULL;

  fl->begin = (uint8_t*)fl->head;
  fl->end = buffer+size;

  fl->size = distance(fl->begin, fl->end);
  return fl;
}

uint8_t* fl_pop(freelist* fl, fl_node* prev, fl_node* curr) {
  if (prev != NULL) {
    prev->next = curr->next;
  } else {
    fl->head = curr->next;
  }
  return (uint8_t*)curr;
}

uint8_t* fl_split(freelist* fl, fl_node* prev, fl_node* curr, size_t requested_size) {
  fl_node* newnode;

  newnode = FL_OFFSETNODE(curr, requested_size);
  newnode->size = curr->size - requested_size;
  newnode->next = curr->next;

  curr->size = requested_size;
  curr->next = newnode;

  return fl_pop(fl, prev, curr);
}

void fl_getnode(freelist* fl, size_t size, uint8_t** outptr, size_t* allocsize) {
  fl_node* curr;
  fl_node* prev;

  if (fl->head == NULL) {
    *outptr = NULL;
    *allocsize = 0;
    return;
  }

  curr = fl->head;
  prev = NULL;

  while (curr != NULL) {
    if (curr->size == size) {
      *outptr = fl_pop(fl, prev, curr);
      *allocsize = size;
      return;
    }

    if (curr->size > size) {
      /* if we allocate an object and the remaining
       * size is not sufficient for a node,
       * we allocate the full space, without splitting
       */
      if (curr->size - size < sizeof(fl_node)) {
        *outptr = fl_pop(fl, prev, curr);
        *allocsize = curr->size;
        return;
      }
      *outptr = fl_split(fl, prev, curr, size);
      *allocsize = size;
      return;
    }

    prev = curr;
    curr = curr->next;
  }

  *outptr = NULL;
  *allocsize = 0;
  return;
}

void* fl_alloc(freelist* fl, size_t size) {
  uint8_t* p;
  size_t allocsize;

  size = fl_pad(size);

  fl_getnode(fl, size, &p, &allocsize);
  if (p == NULL) {
    return NULL;
  }
  ((fl_obj_header*) p)->size = allocsize;
  p += sizeof(fl_obj_header);
  return p;
}

void fl_append(fl_node* prev, fl_node* new) {
  if (FL_OFFSETNODE(prev, prev->size) == new) {
    /* coalescing: append */
    prev->size = prev->size + new->size;
    return;
  }
  prev->next = new;
  new->next = NULL;
  return;
}

void fl_prepend(freelist* fl, fl_node* new) {
  if (FL_OFFSETNODE(new, new->size) == fl->head) {
    /* coalescing: prepend */
    new->size = new->size + fl->head->size;
    new->next = fl->head->next;
    fl->head = new;
    return;
  }

  new->next = fl->head;
  fl->head = new;
  return;
}

void fl_join(fl_node* prev, fl_node* new, fl_node* curr) {
  size_t size;

  if (FL_OFFSETNODE(prev, prev->size) == new) {
    /* coalescing: append */
    size = prev->size + new->size;

    if (FL_OFFSETNODE(prev, size) == curr) {
			/* in this case, prev, new and curr are adjacent */
			prev->size = size + curr->size;
			prev->next = curr->next;
			return;
    }
    /* here only prev and new are adjacent */
    prev->size = size;
    return;
  }

  if (FL_OFFSETNODE(new, new->size) == curr) {
    /* coalescing: prepend */
    prev->next = new;
    new->size = new->size + curr->size;
    new->next = curr->next;
    return;
  }

  prev->next = new;
  new->next = curr;
  return;
}

size_t fl_objsize(const void* ptr) {
  fl_obj_header* obj = (fl_obj_header*)((uint8_t*)ptr - sizeof(fl_obj_header));
  return obj->size;
}

enum fl_RES fl_free(freelist* fl, void* p) {
  size_t size;
  fl_node* new; fl_node* prev; fl_node* curr;
  uint8_t* obj = (uint8_t*)p;

  if (obj < fl->begin || fl->end < obj) {
    return fl_ERR_BOUNDS;
  }

  size = fl_objsize(obj);
  new = (fl_node*)(obj-sizeof(fl_obj_header));

  new->size = size;
  new->next = NULL;

  if (fl->head == NULL) {
    fl->head = new;
    return fl_OK;
  }

  if (new < fl->head) {
    fl_prepend(fl, new);
    return fl_OK;
  }

  prev = NULL;
  curr = fl->head;

  while (curr != NULL) {
    if (prev != NULL) {
      if (prev < new && new < curr) {
        /* in this case, 'new' is a middle node */
        fl_join(prev, new, curr);
        return fl_OK;
      }
    }
    
    prev = curr;
    curr = curr->next;
  }

  /* in this case, 'new' is the last node */
  fl_append(prev, new);
  return fl_OK;
}

void fl_free_all(freelist* fl) {
  fl->head = (fl_node*)fl->begin;
  fl->head->size = fl->size;
  fl->head->next = NULL;
}

size_t fl_available(const freelist* fl) {
  fl_node* curr = fl->head;
  size_t total = 0;

  while (curr != NULL) {
    total += curr->size;
    curr = curr->next;
  }
  return total;
}

size_t fl_used(const freelist* fl) {
  return fl->size - fl_available(fl);
}

bool fl_empty(const freelist* fl) {
  return fl_available(fl) == fl->size;
}

/*
 * -------------------------------------
 * |            ###STACK###             |
 * -------------------------------------
 */

enum sf_RES {sf_OK, sf_SMALLBUFF, sf_STACKEMPTY};

char* sf_str_res(enum sf_RES res);

typedef struct {
  uint8_t* buff;
  size_t allocated;
  size_t chunksize;
  size_t buffsize;
} stack_f;

stack_f* sf_create(uint8_t* buff, size_t buffsize, size_t chunksize, enum sf_RES* res);

uint8_t* sf_alloc(stack_f* sf);

enum sf_RES sf_free(stack_f* sf);

void sf_free_all(stack_f* sf);

size_t sf_available(const stack_f* sf);

size_t sf_used(const stack_f* sf);

bool sf_empty(const stack_f* sf);

char* sf_str_res(enum sf_RES res) {
  switch (res) {
    case sf_OK:
      return "OK";
    case sf_SMALLBUFF:
      return "Provided buffer is too small";
    case sf_STACKEMPTY:
      return "Stack is empty";
  }
  return "???";
}

stack_f* sf_create(uint8_t* buff, size_t buffsize, size_t chunksize, enum sf_RES* res) {
  stack_f* sf;
  if (buffsize < sizeof(stack_f)) {
    *res = sf_SMALLBUFF;
    return NULL;
  }

  sf = (stack_f*)buff;
  sf->buff = buff+sizeof(stack_f);
  sf->chunksize = chunksize;
  sf->buffsize = buffsize-sizeof(stack_f);

  *res = sf_OK;
  return sf;
}

uint8_t* sf_alloc(stack_f* sf) {
  uint8_t* out = sf->buff + sf->allocated;
  sf->allocated += sf->chunksize;
  return out;
}

enum sf_RES sf_free(stack_f* sf) {
  if (sf->allocated == 0) {
    return sf_STACKEMPTY;
  }
  sf->allocated -= sf->chunksize;
  return sf_OK;
}

void sf_free_all(stack_f* sf) {
  sf->allocated = 0;
}

size_t sf_available(const stack_f* sf) {
  return sf->buffsize - sf->allocated;
}

size_t sf_used(const stack_f* sf) {
  return sf->allocated;
}

bool sf_empty(const stack_f* sf) {
  return sf->allocated == 0;
}

/*
 * -------------------------------------
 * |          ###HASHMAP###            |
 * -------------------------------------
 */

/* we need a str->ptr hashmap for the symbol table */

/*
 * -------------------------------------
 * |            ###LEXER###            |
 * -------------------------------------
 */


enum lex_kind {
  lk_bad,
  lk_quote,
  lk_left_parens,
  lk_right_parens,
  lk_bool,
  lk_num,
  lk_str,
  lk_id,
  lk_nil,
  lk_eof
};

enum val_kind {
  vk_none,
  vk_exact_num,
  vk_inexact_num,
  vk_boolean
};

typedef union {
  uint64_t exact_num;
  double   inexact_num;
  bool     boolean;
} lex_value;

typedef struct {
  enum lex_kind kind;
  enum val_kind vkind;

  int16_t begin;
  int16_t end;

  lex_value value;
} lexeme;

typedef struct {
  const char* input;
  lexeme lexeme;
  error err;
} lexer;

lexer new_lexer(const char* input) {
  lexer l;
  l.input = input;
  l.lexeme.begin = 0;
  l.lexeme.end = 0;
  l.lexeme.vkind = vk_none;
  l.lexeme.kind = lk_bad;
  return l;
}

error lex_err_bad_rune(lexer* l) {
  error err;
  err.code = error_bad_rune;
  err.range.begin = l->lexeme.begin;
  err.range.end = l->lexeme.end;
  return err;
}

error lex_err_internal(lexer* l) {
  error err;
  err.code = error_internal_lexer;
  err.range.begin = l->lexeme.begin;
  err.range.end = l->lexeme.end;
  return err;
}

rune lex_next_rune(lexer* l) {
  rune r;
  size_t size = utf8_decode(l->input + l->lexeme.end, &r);
  if (size == 0 || r == -1) {
    l->err = lex_err_bad_rune(l);
    return -1;
  }
  l->lexeme.end += size;
  return r;
}

rune lex_peek_rune(lexer* l) {
  rune r;
  size_t size = utf8_decode(l->input + l->lexeme.end, &r);
  if (size == 0 || r == -1) {
    l->err = lex_err_bad_rune(l);
    return -1;
  }
  return r;
}

void lex_ignore(lexer* l) {
  l->lexeme.begin = l->lexeme.end;
  l->lexeme.kind = lk_bad;
}

typedef bool (*validator)(rune);

bool lex_is_decdigit(rune r) {
  return (r >= '0' && r <= '9') ||
         (r == '_');
}

bool lex_is_hexdigit(rune r) {
  return (r >= '0' && r <= '9') ||
         (r >= 'a' && r <= 'f') ||
         (r >= 'A' && r <= 'F') ||
         (r == '_');
}

bool lex_is_bindigit(rune r) {
  return (r == '0') ||
         (r == '1') ||
         (r == '_');
}

bool lex_is_idchar(rune r) {
  return (r >= 'a' && r <= 'z') ||
         (r >= 'A' && r <= 'Z') ||
         (r == '~') || (r == '+') ||
         (r == '-') || (r == '_') ||
         (r == '*') || (r == '/') ||
         (r == '?') || (r == '=') ||
         (r == '&') || (r == '$') ||
         (r == '%') || (r == '<') ||
         (r == '>') || (r == '!');
}

bool lex_is_idcharnum(rune r) {
  return (r >= '0' && r <= '9') ||
         (r >= 'a' && r <= 'z') ||
         (r >= 'A' && r <= 'Z') ||
         (r == '~') || (r == '+') ||
         (r == '-') || (r == '_') ||
         (r == '*') || (r == '/') ||
         (r == '?') || (r == '=') ||
         (r == '&') || (r == '$') ||
         (r == '%') || (r == '<') ||
         (r == '>') || (r == '!');
}

bool lex_is_whitespace(rune r) {
  return (r == '\n') || (r == '\r') || (r == '\t');
}

bool lex_is_special_str_char(rune r) {
  return (r == '\\') || (r == '"');
}

bool lex_accept_run(lexer* l, validator v) {
  rune r = lex_peek_rune(l);
  if (r < 0) {
    return false;
  }
  while (v(r)) {
    lex_next_rune(l);
    r = lex_peek_rune(l);
    if (r < 0) {
      return false;
    }
  }

  return true;
}

bool lex_accept_until(lexer* l, validator v) {
  rune r = lex_peek_rune(l);
  if (r < 0) {
    return false;
  }
  while (!v(r)) {
    lex_next_rune(l);
    r = lex_peek_rune(l);
    if (r < 0) {
      return false;
    }
  }

  return true;
}

bool lex_read_strlit(lexer* l) {
  rune r = lex_peek_rune(l);
  bool ok;
  if (r != '"') {
    l->err = lex_err_internal(l);
    return false;
  }

  while (true) {
    ok = lex_accept_until(l, lex_is_special_str_char);
    if (ok == false) {
      return false;
    }

    if (r == '"') {
      lex_next_rune(l);
      return true;
    }

    if (r == '\\') {
      lex_next_rune(l);
      ok = lex_next_rune(l);
      if (ok == false) {
        return false;
      }
    }
  }
}

bool lex_conv_hex(lexer* l, uint64_t* value) {
  /* jump the '0x' */
  char* begin = (char*)l->input + l->lexeme.begin + 2;
  char* end = (char*)l->input + l->lexeme.end;
  char c;
  uint64_t output = 0;
  while (begin < end) {
    c = *begin;
    if (c == '_') {
      continue;
    }
    output *= 16;
    if (c >= '0' && c <= '9') {
      output += (c - '0');
    } else if (c >= 'a' && c <= 'z') {
      output += (c - 'a') + 10;
    } else if (c >= 'A' && c <= 'Z') {
      output += (c - 'A') + 10;
    } else {
      l->err = lex_err_internal(l);
      return false;
    }
  }
  *value = output;
  return true;
}

bool lex_conv_bin(lexer* l, uint64_t* value) {
  /* jump the '0b' */
  char* begin = (char*)l->input + l->lexeme.begin + 2;
  char* end = (char*)l->input + l->lexeme.end;
  char c;
  uint64_t output = 0;
  while (begin < end) {
    c = *begin;
    if (c == '_') {
      continue;
    }
    output *= 2;
    if (c == '0' || c == '1') {
      output += (c - '0');
    } else {
      l->err = lex_err_internal(l);
      return false;
    }
  }
  *value = output;
  return true;
}

bool lex_conv_dec(lexer* l, uint64_t* value) {
  char* begin = (char*)l->input + l->lexeme.begin;
  char* end = (char*)l->input + l->lexeme.end;
  char c;
  uint64_t output = 0;
  while (begin < end) {
    c = *begin;
    if (c == '_') {
      continue;
    }
    output *= 10;
    if (c >= '0' && c <= '9') {
      output += (c - '0');
    } else {
      l->err = lex_err_internal(l);
      return false;
    }
  }
  *value = output;
  return true;
}

bool lex_conv_inexact(lexer* l, double* value) {
  char* begin = (char*)l->input + l->lexeme.begin;
  char* end = (char*)l->input + l->lexeme.end;
  char c;
  int fractional = 0;
  double output = 0;
  while (begin < end) {
    c = *begin;
    if (c == '_') {
      continue;
    }
    if (c == '.') {
      fractional += 1;
      continue;
    }

    if (c >= '0' && c <= '9' && fractional == 0) {
      output *= 10;
      output += (c - '0');
    } else if (c >= '0' && c <= '9' && fractional > 0) {
      output += (double)(c - '0') / (10.0*fractional);
      fractional += 1;
    } else {
      l->err = lex_err_internal(l);
      return false;
    }
  }
  *value = output;
  return true;
}

bool lex_read_number(lexer* l) {
  rune r = lex_peek_rune(l);
  bool ok;
  uint64_t exact_value;
  double inexact_value;
  if (r < 0) {
    return false;
  }
  if (r == '0') {
    lex_next_rune(l);
    r = lex_peek_rune(l);
    switch (r) {
      case 'x':
        lex_next_rune(l);
        ok = lex_accept_run(l, lex_is_hexdigit);
        if (ok == false) {
          return false;
        }
        ok = lex_conv_hex(l, &exact_value);
        if (ok == false) {
          return false;
        }
        l->lexeme.value.exact_num = exact_value;
        l->lexeme.vkind = vk_exact_num;
        l->lexeme.kind = lk_num;
        return true;
      case 'b':
        lex_next_rune(l);
        ok = lex_accept_run(l, lex_is_bindigit);
        if (ok == false) {
          return false;
        }
        ok = lex_conv_bin(l, &exact_value);
        if (ok == false) {
          return false;
        }
        l->lexeme.value.exact_num = exact_value;
        l->lexeme.vkind = vk_exact_num;
        l->lexeme.kind = lk_num;
        return true;
    }
  }
  ok = lex_accept_run(l, lex_is_decdigit);
  if (ok == false) {
    return false;
  }
  r = lex_peek_rune(l);
  if (r == '.') {
    lex_next_rune(l);
    ok = lex_accept_run(l, lex_is_decdigit);
    if (ok == false) {
      return false;
    }
    ok = lex_conv_inexact(l, &inexact_value);
    if (ok == false) {
      return false;
    }
    l->lexeme.value.inexact_num = inexact_value;
    l->lexeme.vkind = vk_inexact_num;
    l->lexeme.kind = lk_num;
  } else {
    ok = lex_conv_dec(l, &exact_value);
    if (ok == false) {
      return false;
    }
    l->lexeme.value.exact_num = exact_value;
    l->lexeme.vkind = vk_exact_num;
    l->lexeme.kind = lk_num;
  }
  return true;
}

/* We could get fancy here and implement strcomp and have cleaner code
 * but that would be slower,
 * besides, we only have 3 keywords anyway.
 */
bool lex_is_nil(const lexer* l) {
  char* buffer;
  if (l->lexeme.end - l->lexeme.begin != 3) {
    false;
  }
  buffer = (char*)l->input + l->lexeme.begin;
  return buffer[0] == 'n' &&
         buffer[1] == 'i' &&
         buffer[2] == 'l';
}

bool lex_is_true(const lexer* l) {
  char* buffer;
  if (l->lexeme.end - l->lexeme.begin != 4) {
    false;
  }
  buffer = (char*)l->input + l->lexeme.begin;
  return buffer[0] == 't' &&
         buffer[1] == 'r' &&
         buffer[2] == 'u' &&
         buffer[3] == 'e';
}

bool lex_is_false(const lexer* l) {
  char* buffer;
  if (l->lexeme.end - l->lexeme.begin != 5) {
    false;
  }
  buffer = (char*)l->input + l->lexeme.begin;
  return buffer[0] == 'f' &&
         buffer[1] == 'a' &&
         buffer[2] == 'l' &&
         buffer[3] == 's' &&
         buffer[4] == 'e';
}

bool lex_is_bool(const lexer* l) {
  return lex_is_true(l) || lex_is_false(l);
}

/* returns false if there was an error */
bool lex_conv_bool(lexer* l, bool* value) {
  if (lex_is_false(l)) {
    *value = false;
    return true;
  }
  if (lex_is_true(l)) {
    *value = true;
    return true;
  }
  l->err = lex_err_internal(l);
  return false;
}

bool lex_read_identifier(lexer* l) {
  rune r = lex_peek_rune(l);
  bool ok; bool value;
  if (lex_is_idchar(r) == false){
    l->err = lex_err_internal(l);
    return false;
  }
  l->lexeme.kind = lk_id;
  ok = lex_accept_run(l, lex_is_idcharnum);
  if (ok == false) {
    return false;
  }
  if (lex_is_nil(l)) {
    l->lexeme.kind = lk_nil;
  }
  if (lex_is_bool(l)) {
    ok = lex_conv_bool(l, &value);
    if (ok == false) {
      return false;
    }
    l->lexeme.value.boolean = value;
    l->lexeme.vkind = vk_boolean;
    l->lexeme.kind = lk_bool;
  }
  return true;
}

bool lex_read_comment(lexer* l) {
  rune r = lex_peek_rune(l);
  if (r != '#') {
    /* this should never happen */
    l->err = lex_err_internal(l);
    return false;
  }

  while (r != '\n' && r != EoF) {
    lex_next_rune(l);
    r = lex_peek_rune(l);
    if (r < 0) {
      return false;
    }
  }
  lex_next_rune(l);
  return true;
}

bool lex_ignore_whitespace(lexer* l) {
  rune r = lex_peek_rune(l);
  bool ok;
  if (r < 0) {
    return false;
  }
  while (true) {
    if (lex_is_whitespace(r)) {
      lex_next_rune(l);
    } else if (r == '#') {
      ok = lex_read_comment(l);
      if (ok == false) {
        return false;
      }
    }

    r = lex_peek_rune(l);
    if (r < 0) {
      return false;
    }
  }
  lex_ignore(l);
}

bool lex_read_any(lexer* l) {
  rune r;
  bool ok = lex_ignore_whitespace(l);
  if (ok == false) {
    return false;
  }

  r = lex_peek_rune(l);
  if (lex_is_decdigit(r)) {
    return lex_read_number(l);
  }
  if (lex_is_idchar(r)){
    return lex_read_identifier(l);
  }
  switch (r) {
    case '"':
      return lex_read_strlit(l);
    case '(':
      lex_next_rune(l);
      l->lexeme.kind = lk_left_parens;
      break;
    case ')':
      lex_next_rune(l);
      l->lexeme.kind = lk_right_parens;
      break;
    case '\'':
      lex_next_rune(l);
      l->lexeme.kind = lk_quote;
      break;
  }
  return true;
}

/*
 * it returns true if everything went smoothly,
 * and returns false if there was some error.
 * the error is stored in lexer.err
 */
bool lex_next(lexer* l) {
  l->lexeme.begin = l->lexeme.end;
  return lex_read_any(l);
}

bool lex_read_all(lexer* l, lexeme* outbuffer, size_t outbuffer_size) {
  size_t i = 0;
  while (lex_next(l) && i < outbuffer_size) {
    outbuffer[i] = l->lexeme;
    i++;
  }
  if (l->lexeme.kind != lk_eof) {
    return false;
  }
  return true;
}

/*
 * -------------------------------------
 * |           ###PARSER###            |
 * -------------------------------------
 */

typedef enum {
  tbi_null,
  tbi_eelist,  /* expr _exprlist   */
  tbi_elist,   /* exprlist         */
  tbi_qexpr,   /* quote _expr      */
  tbi_quote,   /* "'" quote        */
  tbi_empty,   /* \e               */
  tbi_atom,    /* atom             */
  tbi_bool,    /* bool             */
  tbi_num,     /* num              */
  tbi_nil,     /* "nil"            */
  tbi_id,      /* id               */
  tbi_str,     /* str              */
  tbi_list,    /* list             */
  tbi_thelist  /* "(" exprlist ")" */
} parser_table_item;

parser_table_item parser_parsing_table[7][8] =
{
/*               '           (            bool        num         str         id          nil         eof */
/* exprlist  */ {tbi_eelist, tbi_eelist,  tbi_eelist, tbi_eelist, tbi_eelist, tbi_eelist, tbi_eelist, tbi_null},
/* _exprlist */ {tbi_elist,  tbi_elist,   tbi_elist,  tbi_elist,  tbi_elist,  tbi_elist,  tbi_elist,  tbi_empty},
/* expr      */ {tbi_qexpr,  tbi_qexpr,   tbi_qexpr,  tbi_qexpr,  tbi_qexpr,  tbi_qexpr,  tbi_qexpr,  tbi_null},
/* quote     */ {tbi_quote,  tbi_empty,   tbi_empty,  tbi_empty,  tbi_empty,  tbi_empty,  tbi_empty,  tbi_null},
/* _expr     */ {tbi_null,   tbi_list,    tbi_atom,   tbi_atom,   tbi_atom,   tbi_atom,   tbi_atom,   tbi_null},
/* atom      */ {tbi_null,   tbi_null,    tbi_bool,   tbi_num,    tbi_str,    tbi_id,     tbi_nil,    tbi_null},
/* list      */ {tbi_null,   tbi_thelist, tbi_null,   tbi_null,   tbi_null,   tbi_null,   tbi_null,   tbi_null},
};

enum parser_prodkind {
  pk_exprlist,
  pk__exprlist,
  pk_expr,
  pk__expr,
  pk_quote,
  pk_atom,
  pk_list
};

typedef struct {
  enum lex_kind lkind;
  enum parser_prodkind pkind;
  bool isTerminal;
} parser_stack_item;

/*
 * -------------------------------------
 * |      ###BUILTIN FUNCTIONS###      |
 * -------------------------------------
 */

/*
 * -------------------------------------
 * |         ###EVALUATOR###           |
 * -------------------------------------
 */

