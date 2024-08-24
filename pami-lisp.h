#include <stdint.h>
#include <stdbool.h>

struct datum;

typedef struct {
  int16_t start;
  int16_t len;
  char* buff;
} str;

typedef struct {
  str name;
} symbol;

typedef struct {
  struct datum* car;
  struct datum* cdr;
} pair;

typedef struct datum*(*lambda)(struct datum*);
typedef struct datum*(*cproc)(struct datum*);

enum datum_tag {
  EXACT_NUM, INEXACT_NUM,
  BOOL, STRING, LAMBDA,
  C_PROC, SYMBOL, PAIR
};

typedef union {
  int64_t exact_num;
  double inexact_num;
  bool boolean;
  str string;
  pair pair;
  cproc cproc;
  lambda lambda;
  symbol symbol;
} datum_union;

typedef struct datum {
  enum datum_tag tag;
  datum_union data;
} datum;
