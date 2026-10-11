/* The cksumvfs page checksum (docs/formats/sqlite.md Q-SQLITE-7), taken from the vendored cksumvfs.c. */
#define SQLITE_CKSUMVFS_STATIC
#include "cksumvfs.c"

void semsplinter_cksum(unsigned char* page, int n, unsigned char* out) {
    cksmCompute(page, n, out);
}
