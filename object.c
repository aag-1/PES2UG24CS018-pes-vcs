// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

/* ── Stack size adjustment ─────────────────────────────────────────────────
 * The Index struct (10000 × IndexEntry) is ~5.6 MB and is allocated on the
 * stack by pes.c (which we cannot modify).  Bump the soft stack limit to
 * 64 MB at process start so cmd_add / cmd_status do not segfault.
 */
__attribute__((constructor))
static void bump_stack_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        if (rl.rlim_cur < 64 * 1024 * 1024) {
            rl.rlim_cur = 64 * 1024 * 1024;
            setrlimit(RLIMIT_STACK, &rl);
        }
    }
}


// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    /* Step 1: Build header string: "<type> <size>\0" */
    const char *type_str = (type == OBJ_BLOB) ? "blob"
                         : (type == OBJ_TREE) ? "tree"
                         : "commit";
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
    /* +1 includes the '\0' terminator that snprintf writes */

    /* Step 2: Construct full object = header + data */
    size_t total = (size_t)header_len + len;
    uint8_t *full = malloc(total);
    if (!full) return -1;
    memcpy(full, header, (size_t)header_len);
    memcpy(full + header_len, data, len);

    /* Step 3: Compute SHA-256 of full object */
    compute_hash(full, total, id_out);

    /* Step 4: Deduplication — skip if already stored */
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    /* Step 5: Build shard directory path and create it */
    char path[512];
    object_path(id_out, path, sizeof(path));

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    /* dir is "…/XX/YYYY…" — strip filename to get directory */
    char *slash = strrchr(dir, '/');
    if (!slash) { free(full); return -1; }
    *slash = '\0';
    mkdir(dir, 0755); /* ignore EEXIST */

    /* Step 6: Write to temp file in shard directory */
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s/tmp_XXXXXX", dir);
    int fd = mkstemp(tmp);
    if (fd < 0) { free(full); return -1; }

    /* Step 7: Write full object, fsync, close */
    ssize_t written = write(fd, full, total);
    free(full);
    if (written < 0 || (size_t)written != total) {
        close(fd); unlink(tmp); return -1;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);

    /* Step 8: Atomic rename temp → final path */
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    /* Step 9: fsync shard directory to persist the rename */
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    /* Step 1: Resolve file path */
    char path[512];
    object_path(id, path, sizeof(path));

    /* Step 2: Read entire file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    /* Step 3 & 4: Integrity check — recompute hash and compare */
    ObjectID computed;
    compute_hash(buf, (size_t)fsize, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf); return -1; /* Corrupt object */
    }

    /* Step 5: Find '\0' separating header from data */
    uint8_t *null_pos = memchr(buf, '\0', (size_t)fsize);
    if (!null_pos) { free(buf); return -1; }

    /* Parse type from header prefix */
    if (strncmp((char *)buf, "blob ", 5) == 0)   *type_out = OBJ_BLOB;
    else if (strncmp((char *)buf, "tree ", 5) == 0)  *type_out = OBJ_TREE;
    else if (strncmp((char *)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    /* Step 6: Copy data portion (after '\0') into caller-owned buffer */
    uint8_t *data_start = null_pos + 1;
    size_t data_len = (size_t)fsize - (size_t)(data_start - buf);

    uint8_t *out = malloc(data_len + 1); /* +1 for safe null termination */
    if (!out) { free(buf); return -1; }
    memcpy(out, data_start, data_len);
    out[data_len] = '\0';

    free(buf);
    *data_out = out;
    *len_out  = data_len;
    return 0;
}
