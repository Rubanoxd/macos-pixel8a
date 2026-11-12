// Fill N MiB with an address-derived pattern, wait, verify; report mismatches. Usage: memcheck <MiB> <seconds> <rounds>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
int main(int c, char **v) {
  size_t mib = c > 1 ? atol(v[1]) : 1024; int wait = c > 2 ? atoi(v[2]) : 30, rounds = c > 3 ? atoi(v[3]) : 3;
  size_t n = mib << 17;  // uint64 words
  uint64_t *p = mmap(0, n * 8, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); return 1; }
  for (int r = 0; r < rounds; r++) {
    uint64_t salt = 0x9E3779B97F4A7C15ULL * (r + 1);
    for (size_t i = 0; i < n; i++) p[i] = (i * 0xD6E8FEB86659FD93ULL) ^ salt;
    sleep(wait);
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) if (p[i] != ((i * 0xD6E8FEB86659FD93ULL) ^ salt)) { if (bad < 5) printf("bad word %zu: %016llx\n", i, (unsigned long long)p[i]); bad++; }
    printf("round %d: %zu MiB, %zu bad words\n", r, mib, bad); fflush(stdout);
  }
  return 0;
}
