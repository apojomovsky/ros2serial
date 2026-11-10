// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Coverage driver: feeds each named seed file to LLVMFuzzerTestOneInput and
 * exits nonzero on the first unreadable file. The libFuzzer build measures
 * crashes, not coverage; this gcc --coverage build replays the committed
 * corpus so gcovr can say which lines each target's seeds reach. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint8_t file_bytes[65535];

int main(int argc, char **argv)
{
  int i;

  if (argc < 2) {
    fprintf(stderr, "usage: cov_driver SEED...\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    FILE *file = fopen(argv[i], "rb");
    size_t len;

    if (file == NULL) {
      fprintf(stderr, "cov_driver: cannot open %s\n", argv[i]);
      return 1;
    }
    len = fread(file_bytes, 1u, sizeof file_bytes, file);
    fclose(file);
    if (len == 0u || len == sizeof file_bytes) {
      fprintf(stderr, "cov_driver: cannot replay %s\n", argv[i]);
      return 1;
    }
    LLVMFuzzerTestOneInput(file_bytes, len);
  }
  return 0;
}
