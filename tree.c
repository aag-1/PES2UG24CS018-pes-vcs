// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* Forward declarations from object.c */
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

/* ── Index types needed by tree_from_index ──────────────────────────────────
 * Include the header for type definitions, but provide a weak fallback for
 * index_load so that test_tree can link without index.o.
 * The real index_load from index.o overrides the weak stub in the pes binary. */
#include "index.h"

__attribute__((weak))
int index_load(Index *index) { index->count = 0; return 0; }

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
/* Forward declaration for recursive helper */
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out);

/*
 * write_tree_level — Recursively build one level of the tree.
 *
 * 'entries'  : array of IndexEntry, each with path relative to repo root
 * 'count'    : number of entries in that array
 * 'prefix'   : directory prefix for this level (e.g. "src/" or "" for root)
 * 'id_out'   : receives the hash of the written tree object
 *
 * Strategy:
 *   Walk the entries in order.  For each entry whose path starts with
 *   'prefix', strip the prefix to get the relative name.
 *   - If the relative name has no '/', it is a file at this level → add blob entry.
 *   - If the relative name has a '/', the first component is a subdirectory.
 *     Collect all entries sharing that subdirectory prefix, recurse, then add
 *     the resulting tree entry.
 */
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out)
{
    Tree tree;
    tree.count = 0;
    size_t prefix_len = strlen(prefix);

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;

        /* Skip entries that do not belong to this prefix */
        if (strncmp(path, prefix, prefix_len) != 0) { i++; continue; }

        const char *rel = path + prefix_len; /* path relative to this level */

        char *slash = strchr(rel, '/');
        if (slash == NULL) {
            /* ── File entry at this level ── */
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            strncpy(e->name, rel, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = entries[i].hash;
            i++;
        } else {
            /* ── Subdirectory: collect all entries sharing this sub-prefix ── */
            size_t dir_name_len = (size_t)(slash - rel);
            char sub_name[256];
            if (dir_name_len >= sizeof(sub_name)) return -1;
            memcpy(sub_name, rel, dir_name_len);
            sub_name[dir_name_len] = '\0';

            /* Build full sub-prefix: prefix + dir_name + "/" */
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, sub_name);
            size_t sub_prefix_len = strlen(sub_prefix);

            /* Count entries that belong to this subdirectory */
            int sub_start = i;
            while (i < count &&
                   strncmp(entries[i].path, sub_prefix, sub_prefix_len) == 0) {
                i++;
            }

            /* Recurse to build the subtree */
            ObjectID sub_id;
            if (write_tree_level(entries + sub_start, i - sub_start,
                                  sub_prefix, &sub_id) != 0)
                return -1;

            /* Add tree entry for the subdirectory */
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            strncpy(e->name, sub_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = sub_id;
        }

        if (tree.count >= MAX_TREE_ENTRIES) break;
    }

    /* Serialize and write tree object */
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) {
        /* Empty index — write an empty tree */
        Tree empty; empty.count = 0;
        void *d; size_t l;
        if (tree_serialize(&empty, &d, &l) != 0) return -1;
        int rc = object_write(OBJ_TREE, d, l, id_out);
        free(d);
        return rc;
    }
    return write_tree_level(index.entries, index.count, "", id_out);
}