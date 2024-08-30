#include <stdio.h>
#include <stdlib.h>
#include "pami-lisp.c"

char* utf8_test_data = "\x68\U00000393\U000030AC\U000101FA";

void check_rune(char** curr_char, rune expected) {
  rune r;
  size_t rune_size;
  rune_size = utf8_decode(*curr_char, &r);
  if (rune_size > 0) {
    *curr_char += rune_size;
    if (r != expected) {
      printf("runes don't match: U+%X != U+%X\n", r, expected);
      abort();
    }
  } else {
    printf("invalid rune, expected U+%X\n", expected);
    abort();
  }
}

/* very weak test, but can be improved later */
void utf8_test() {
  char* curr_char = utf8_test_data;
  check_rune(&curr_char, 0x68);
  check_rune(&curr_char, 0x0393);
  check_rune(&curr_char, 0x30AC);
  check_rune(&curr_char, 0x101FA);
  printf("utf8_test: OK\n");
}

int main() {
  utf8_test();
  return 0;
}
