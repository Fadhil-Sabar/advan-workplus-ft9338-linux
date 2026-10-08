/* Hardware-free smoke test for the native engine loader/preparer. */
#include "../src/ft_engine.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: %s path/to/ftWbioEngineAdapter.dll\n", argv[0]);
    return 2;
  }
  int result = ft_engine_open(argv[1]);
  if (result != 0) {
    fprintf(stderr, "ft_engine_open failed: %d\n", result);
    return 1;
  }
  uint8_t frame[64 * 80] = {0};
  uint32_t accept_status = ft_engine_accept(frame, 64, 80, 0);
  fprintf(stderr, "loader initialized; blank-frame accept status=%#x\n", accept_status);
  return 0;
}
