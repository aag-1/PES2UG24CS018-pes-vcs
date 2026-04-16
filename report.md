# PES-VCS Lab Report

**Student:** Aayush  
**SRN:** PES2UG24CS018  
**Repository:** https://github.com/aag-1/PES2UG24CS018-pes-vcs  
**Platform:** Ubuntu 22.04 | C (gcc) | OpenSSL SHA-256

---

## Phase 1: Object Storage Foundation

### Screenshot 1A — `./test_objects` output (all tests passing)

```
$ mkdir -p .pes/objects .pes/refs/heads
$ ./test_objects

Stored blob with hash: d58213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
Object stored at: .pes/objects/d5/8213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
PASS: blob storage
PASS: deduplication
PASS: integrity check

All Phase 1 tests passed.
```

**What's being tested:**
- `blob storage`: `object_write` stores a blob; `object_read` returns identical bytes.
- `deduplication`: Writing the same content twice produces the same hash; only one file is stored.
- `integrity check`: Flipping a byte in the stored object causes `object_read` to return `-1` (hash mismatch).

---

### Screenshot 1B — `find .pes/objects -type f` (sharded directory structure)

```
$ find .pes/objects -type f

.pes/objects/d5/8213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
.pes/objects/25/ef1fa07ea68a52f800dc80756ee6b7ae34b337afedb9b46a1af8e11ec4f476
.pes/objects/2a/594d39232787fba8eb7287418aec99c8fc2ecdaf5aaf2e650eda471e566fcf
```

**Structure explanation:**  
Each object is stored under `.pes/objects/<XX>/<62-char-suffix>` where `XX` is the first two hex characters of the SHA-256 hash. This directory sharding prevents any single directory from containing too many files (ext4 `dir_index` B-tree performance degrades beyond ~100K entries per directory; sharding caps subdirectories at ~256 entries × 256 shard dirs).

---

## Phase 2: Tree Objects

### Screenshot 2A — `./test_tree` output (all tests passing)

```
$ ./test_tree

Serialized tree: 139 bytes
PASS: tree serialize/parse roundtrip
PASS: tree deterministic serialization

All Phase 2 tests passed.
```

**What's being tested:**
- `roundtrip`: A tree with 3 entries (README.md, src/, build.sh) serializes and parses back with all modes, hashes, and names preserved.
- `deterministic serialization`: Two `Tree` structs with the same entries in different insertion order produce byte-identical binary output (entries are sorted alphabetically before serialization).

---

### Screenshot 2B — `xxd` of a raw tree object (binary format)

```
$ TREE_HASH="15288745ac657ca050af71d2ff09c7cc3d0cb7c8291a8bfb620d328b60a2b604"
$ xxd .pes/objects/15/288745ac657ca050af71d2ff09c7cc3d0cb7c8291a8bfb620d328b60a2b604 | head -20

00000000: 7472 6565 2039 3800 3130 3036 3434 2068  tree 98.100644 h
00000010: 656c 6c6f 2e74 7874 00c5 8493 426d 4595  ello.txt....BmE.
00000020: 0cfb 2462 3388 a0f9 545c bb33 905e 58ed  ..$b3...T\.3.^X.
00000030: ad80 2c0f 1dd1 1929 0131 3030 3634 3420  ..,....).100644
00000040: 776f 726c 642e 7478 7400 0b30 1410 e2dc  world.txt..0....
00000050: 3cb0 0d5d 4791 9e42 6754 26e9 9e2c a217  <..]G..BgT&..,.
00000060: cbc5 a66e 6616 b4bc 965a                 ...nf....Z
```

**Format breakdown:**
- Bytes `0x00–0x07`: Object header `"tree 98\0"` — type string + space + decimal size of data + null byte
- Entry 1 starts at `0x08`: `"100644 hello.txt\0"` (ASCII octal mode + space + filename + null), followed by **32 raw binary bytes** of SHA-256 hash
- Entry 2: `"100644 world.txt\0"` + 32-byte binary hash
- No separator between entries — the null byte after each name and the fixed 32-byte hash act as implicit delimiters

---

## Phase 3: The Index (Staging Area)

### Screenshot 3A — `pes init` → `pes add` → `pes status`

```
$ export PES_AUTHOR="Aayush <PES2UG24CS018>"
$ ./pes init
Initialized empty PES repository in .pes/

$ echo "hello world" > file1.txt
$ echo "version control" > file2.txt
$ ./pes add file1.txt file2.txt
$ ./pes status

Staged changes:
  staged:     file1.txt
  staged:     file2.txt

Unstaged changes:
  (nothing to show)

Untracked files:
  untracked:  tree.h
  untracked:  object.c
  untracked:  commit.c
  untracked:  pes.h
  untracked:  README.md
  untracked:  Makefile
  untracked:  .gitignore
  untracked:  test_tree.c
  untracked:  commit.h
  untracked:  test_objects.c
  untracked:  index.h
  untracked:  index.c
  untracked:  test_sequence.sh
  untracked:  tree.c
  untracked:  pes.c
```

**Workflow explanation:**
1. `pes init` creates `.pes/objects/`, `.pes/refs/heads/`, and `.pes/HEAD` (`ref: refs/heads/main`).
2. `pes add file1.txt file2.txt` reads each file, computes its blob hash via `object_write(OBJ_BLOB)`, captures `stat()` metadata (mtime, size, mode), and persists to `.pes/index` atomically.
3. `pes status` loads the index and compares mtime+size against the working directory for fast change detection without re-hashing.

---

### Screenshot 3B — `cat .pes/index` (text-format index)

```
$ cat .pes/index

100644 0bd69098bd9b9cc5934a610ab65da429b525361147faa7b5b922919e9a23143d 1776360302 12 file1.txt
100644 e5ee91452e3f3a226955b73b7f58012e08ebefa026d3b2984e2ed990c105fa4e 1776360302 16 file2.txt
```

**Format:** `<mode-octal> <64-hex-SHA256> <mtime-unix-seconds> <size-bytes> <path>`

- Entries are sorted alphabetically by path (guaranteed by `qsort` in `index_save`).
- `100644` = regular non-executable file (octal).
- The 64-hex hash identifies the blob object in `.pes/objects/`.
- mtime and size allow fast dirty-detection without re-reading file contents.
- Written atomically: `fopen(.pes/index.tmp)` → `fflush` → `fsync` → `rename` → ensures no partial index visible on crash.

---

## Phase 4: Commits and History

### Screenshot 4A — `pes log` (three commits)

```
$ export PES_AUTHOR="Aayush <PES2UG24CS018>"
$ ./pes commit -m "Initial commit"
[main a863a64] Initial commit
Committed: a863a64bd1e6... Initial commit

$ echo "more content" >> file1.txt && ./pes add file1.txt
$ ./pes commit -m "Update file1.txt with more content"
[main 578f1c3] Update file1.txt with more content
Committed: 578f1c3ba0c5... Update file1.txt with more content

$ echo "goodbye world" > bye.txt && ./pes add bye.txt
$ ./pes commit -m "Add farewell file"
[main 3914cce] Add farewell file
Committed: 3914ccec4300... Add farewell file

$ ./pes log

commit 3914ccec4300cb55bb7bc0ff111d3b708a56bbccd3b196dadbeadf74665c8aa5
Author: Aayush <PES2UG24CS018>
Date:   1776360310

    Add farewell file

commit 578f1c3ba0c5a1cdde89784fdc49928910cd5df8421045ca4adf4f56583ec886
Author: Aayush <PES2UG24CS018>
Date:   1776360309

    Update file1.txt with more content

commit a863a64bd1e668deda87be8c2600ef4565d368632eed392510e7d0c09451b450
Author: Aayush <PES2UG24CS018>
Date:   1776360309

    Initial commit
```

**How `pes log` works:** `commit_walk` reads `HEAD` → resolves the branch symref → reads the commit object → parses it → calls the print callback → follows the `parent` field until a commit with `has_parent=0` is reached.

---

### Screenshot 4B — `find .pes -type f | sort` (object store after three commits)

```
$ find .pes -type f | sort

.pes/HEAD
.pes/index
.pes/objects/0b/d69098bd9b9cc5934a610ab65da429b525361147faa7b5b922919e9a23143d
.pes/objects/36/47f9b8dcb5185f6c1be5e8bd2a1d22644d0d9a7d974b4da7d473207aabfe6f
.pes/objects/39/14ccec4300cb55bb7bc0ff111d3b708a56bbccd3b196dadbeadf74665c8aa5
.pes/objects/4f/e492c39015f8b922b97b404054dea9fd02a5b364703797e4a3a16cf0bff8ac
.pes/objects/57/8f1c3ba0c5a1cdde89784fdc49928910cd5df8421045ca4adf4f56583ec886
.pes/objects/74/1e3ec3b5ac2e19b2a410d9440a4d36c498e24b7f1de5d4fa61d5226bca6972
.pes/objects/84/be7347bfc4400a6e17e7d34854363e60c20a52ac73e70fea5166e351ea5be7
.pes/objects/9e/a70aefabff8570c5ef0020c66b72423fc524b16dbd6da4c8eaf21f5d3a2727
.pes/objects/a8/63a64bd1e668deda87be8c2600ef4565d368632eed392510e7d0c09451b450
.pes/objects/e5/ee91452e3f3a226955b73b7f58012e08ebefa026d3b2984e2ed990c105fa4e
.pes/refs/heads/main
```

**Object breakdown (11 objects for 3 commits):**
- 3 commit objects (one per `pes commit`)
- 3 tree objects (root tree snapshot per commit)
- 4 blob objects (file1.txt-v1, file1.txt-v2, file2.txt, bye.txt — file2.txt reused across commits 1 & 2 via deduplication)
- `.pes/HEAD` and `.pes/index` are metadata files, not objects

---

### Screenshot 4C — Reference chain (`cat .pes/refs/heads/main` and `cat .pes/HEAD`)

```
$ cat .pes/refs/heads/main
3914ccec4300cb55bb7bc0ff111d3b708a56bbccd3b196dadbeadf74665c8aa5

$ cat .pes/HEAD
ref: refs/heads/main
```

**Reference chain explained:**
```
.pes/HEAD  ──(symref)──►  .pes/refs/heads/main  ──(hash)──►  commit 3914cce...
                                                                    │
                                                               parent: 578f1c3...
                                                                    │
                                                               parent: a863a64... (no parent)
```
- `HEAD` is a symbolic reference (`ref: refs/heads/main`) — it always points to the current branch name, not a hash directly.
- `refs/heads/main` holds the raw SHA-256 of the latest commit.
- `head_update()` atomically rewrites `refs/heads/main` via temp-file + rename after every `pes commit`.

---

## Final: Full Integration Test (`make test-integration`)

```
$ export PES_AUTHOR="Aayush <PES2UG24CS018>"
$ make test-integration

=== Running integration tests ===
bash test_sequence.sh
=== PES-VCS Integration Test ===

--- Repository Initialization ---
Initialized empty PES repository in .pes/
PASS: .pes/objects exists
PASS: .pes/refs/heads exists
PASS: .pes/HEAD exists

--- Staging Files ---
Status after add:
Staged changes:
  staged:     file.txt
  staged:     hello.txt

Unstaged changes:
  (nothing to show)

Untracked files:
  (nothing to show)


--- First Commit ---
[main 0989863] Initial commit
Committed: 098986376e29... Initial commit

Log after first commit:
commit 098986376e298dc29a1e84a53a98383de625b46e3b9cbf8d42488234287ff2c9
Author: Aayush <PES2UG24CS018>
Date:   1776360321

    Initial commit


--- Second Commit ---
[main 94bd8b0] Update file.txt
Committed: 94bd8b076c6a... Update file.txt

--- Third Commit ---
[main 1270860] Add farewell
Committed: 127086062a50... Add farewell

--- Full History ---
commit 127086062a502be0c45872beeb25c4d6464a4cd1e39654532a715a1e28cab74b
Author: Aayush <PES2UG24CS018>
Date:   1776360321

    Add farewell

commit 94bd8b076c6a6b95d120d6b8b798282e5f7f4ed68ffcd1344acb78f3037bc300
Author: Aayush <PES2UG24CS018>
Date:   1776360321

    Update file.txt

commit 098986376e298dc29a1e84a53a98383de625b46e3b9cbf8d42488234287ff2c9
Author: Aayush <PES2UG24CS018>
Date:   1776360321

    Initial commit


--- Reference Chain ---
HEAD:
ref: refs/heads/main
refs/heads/main:
127086062a502be0c45872beeb25c4d6464a4cd1e39654532a715a1e28cab74b

--- Object Store ---
Objects created:
10
.pes/objects/09/8986376e298dc29a1e84a53a98383de625b46e3b9cbf8d42488234287ff2c9
.pes/objects/0b/d69098bd9b9cc5934a610ab65da429b525361147faa7b5b922919e9a23143d
.pes/objects/10/1f28a12274233d119fd3c8d7c7216054ddb5605f3bae21c6fb6ee3c4c7cbfa
.pes/objects/12/7086062a502be0c45872beeb25c4d6464a4cd1e39654532a715a1e28cab74b
.pes/objects/58/a67ed1c161a4e89a110968310fe31e39920ef68d4c7c7e0d6695797533f50d
.pes/objects/94/bd8b076c6a6b95d120d6b8b798282e5f7f4ed68ffcd1344acb78f3037bc300
.pes/objects/ab/5824a9ec1ef505b5480b3e37cd50d9c80be55012cabe0ca572dbf959788299
.pes/objects/b1/7b838c5951aa88c09635c5895ef7e08f7fa1974d901ce282f30e08de0ccd92
.pes/objects/d0/7733c25d2d137b7574be8c5542b562bf48bafeaa3829f61f75b8d10d5350f9
.pes/objects/db/07a1451ca9544dbf66d769b505377d765efae7adc6b97b75cc9d2b3b3da6ff

=== All integration tests completed ===
```

---

## Phase 5 & 6: Analysis Questions

---

### Q5.1 — Implementing `pes checkout <branch>`

**What files change in `.pes/`:**

1. **`.pes/HEAD`** — rewrite from `ref: refs/heads/old-branch` to `ref: refs/heads/<branch>`. This is a single atomic `rename()` over a temp file.
2. **Working directory files** — update every tracked file to match the target branch's tree snapshot.

**Step-by-step algorithm:**

```
1. Read target branch ref:  open .pes/refs/heads/<branch>, read commit hash
2. Load target commit:      object_read(commit_hash) -> parse tree hash
3. Load current HEAD tree:  head_read() -> object_read(commit) -> tree hash
4. Diff trees:              compare current tree vs target tree (recursive)
5. For each changed file:
     - If file exists in target:  object_read(blob_hash) + write to disk
     - If file removed in target: unlink() from working directory
6. Update HEAD:             write "ref: refs/heads/<branch>" atomically
7. Update index:            rebuild index from target tree entries
```

**What makes this complex:**

- **Dirty working directory detection** (see Q5.2 below) — must reject or warn before touching any files if uncommitted changes would be overwritten.
- **Partial failure recovery** — if checkout fails halfway (e.g., disk full), the working directory is left in a mixed state. Git handles this by doing a dry-run diff first, then applying changes only if all checks pass.
- **Untracked file conflicts** — if a file exists in the working directory that is tracked in the target branch but not the current one, checkout must refuse to avoid silently overwriting user data.
- **Recursive subtrees** — directory entries in the tree object require recursively reading subtree objects, then creating directories and writing files.
- **Symlinks and executable bits** — mode bits (`100755`, `120000` for symlinks) must be correctly restored via `chmod()` and `symlink()`.

---

### Q5.2 — Detecting dirty working directory conflicts before checkout

**Algorithm (using only index + object store, no working directory diff):**

```
For each entry E in the current index:
  1. stat(E.path) in the working directory
  2. If stat fails (file deleted):
       -> file is modified/deleted in WD, mark as dirty
  3. If st.st_mtime != E.mtime_sec OR st.st_size != E.size:
       -> file likely changed; do a full content diff:
          read file bytes, compute SHA-256, compare to E.hash
          if hashes differ -> file is "dirty" (uncommitted change)

For each file that is dirty:
  Look up E.path in the TARGET branch's tree:
  - If target_blob_hash != E.hash (the file differs between branches):
      -> CONFLICT: checkout would overwrite uncommitted changes -> REFUSE
  - If target_blob_hash == E.hash (same content in both branches):
      -> no conflict; the checkout won't change this file anyway -> OK
```

**Why this is correct:**  
- Fast path: mtime+size match → assume clean (git's strategy — avoids re-hashing every file on large repos).
- Slow path (mtime changed): re-hash to be certain.
- A file is only a problem if it's both (a) dirty in the working directory AND (b) different between the two branches' tree snapshots.
- Files not in the index (untracked) must also be checked against the target tree — if the target tree contains a file at the same path, checkout must refuse to avoid silently overwriting the untracked file.

---

### Q5.3 — Detached HEAD: commits and recovery

**What happens when you commit in detached HEAD state:**

When `HEAD` contains a raw commit hash instead of `ref: refs/heads/main`, `head_update()` writes the new commit hash directly into `HEAD` itself (not into a branch ref file). Each new commit correctly sets `parent` to the previous commit, so a valid chain of commits is created. However, **no branch pointer is updated** — the commits are only reachable via the raw hash in `HEAD`.

**The danger:**  
If the user then runs `pes checkout main`, `HEAD` is overwritten with `ref: refs/heads/main`, and the commits made in detached HEAD state become **unreachable** from any branch. They are not immediately deleted but will be removed by the next garbage collection pass.

**Recovery methods:**

1. **Immediate recovery (before any GC):**  
   The detached HEAD commits still exist in `.pes/objects/`. Read the hash from the old `HEAD` value (e.g., from shell history or a backup), then create a new branch pointing to it:
   ```bash
   cat .pes/HEAD              # shows the commit hash if still detached
   echo "<hash>" > .pes/refs/heads/recovery-branch
   ```

2. **Via reflog (if implemented):**  
   Git maintains `.git/logs/HEAD` — a log of every value HEAD has held. Checking this log reveals the commit hash before the checkout, enabling recovery. PES-VCS does not implement reflog, making recovery harder.

3. **Object store scan:**  
   Since GC has not run, scan `.pes/objects/` for commit objects and manually trace parent chains to find the orphaned commits, then create a branch pointing to the tip.

**Key lesson:** Detached HEAD commits are not "lost" — they are unreferenced but still in the object store until GC runs. Git's default GC grace period is 30 days for unreachable objects, giving time to recover.

---

### Q6.1 — Garbage Collection: Finding and Deleting Unreachable Objects

**Algorithm (Mark-and-Sweep):**

**Phase 1 — Mark (find all reachable objects):**

```
reachable = HashSet()    # O(1) lookup

For each branch file in .pes/refs/heads/:
    start_hash = read(branch_file)
    walk_commit_chain(start_hash, reachable)

def walk_commit_chain(commit_hash, reachable):
    if commit_hash in reachable: return   # already visited
    reachable.add(commit_hash)
    commit = object_read(commit_hash)     # parse commit
    reachable.add(commit.tree)
    walk_tree(commit.tree, reachable)     # add all tree+blob children
    if commit.has_parent:
        walk_commit_chain(commit.parent, reachable)

def walk_tree(tree_hash, reachable):
    if tree_hash in reachable: return
    reachable.add(tree_hash)
    tree = object_read(tree_hash)
    for entry in tree.entries:
        if entry.mode == MODE_DIR:
            walk_tree(entry.hash, reachable)
        else:
            reachable.add(entry.hash)   # blob
```

**Phase 2 — Sweep (delete unreachable):**

```
For each file in .pes/objects/**:
    hash = dirname[-2:] + filename    # reconstruct full hash
    if hash not in reachable:
        unlink(file)
    # Optionally remove empty shard dirs
```

**Data structure:** A `HashSet<ObjectID>` (hash table keyed by the 32-byte SHA-256). Each lookup is O(1) average. A `bool[]` array indexed by first 2 hex chars + a nested hash map for the remaining 62 chars also works.

**Estimate for 100,000 commits, 50 branches:**

Assuming an average project with:
- ~10 files per commit (tree entries), average tree depth 2 → ~15 tree objects per commit
- ~8 unique blobs per commit (most files unchanged = deduplicated)

Objects to visit during mark:
- Commits: `100,000`
- Trees: `100,000 × 15 = 1,500,000`
- Blobs: harder to bound due to dedup — roughly `100,000 × 8 = 800,000` unique blobs in the worst case

**Total reachable objects: ~2,400,000**  
Add ~20% overhead for shared objects visited multiple times (early-exit via `reachable` set): effectively **~3M object_read calls** in the mark phase.

---

### Q6.2 — GC Race Condition with Concurrent Commit

**The race condition:**

```
Time →

Thread A (pes commit):                    Thread B (GC mark phase):
  1. tree_from_index() creates blob X        
  2. object_write(OBJ_BLOB, ...) -> X       
     (X is now in .pes/objects)             
                                          3. GC scans .pes/refs/heads/main
                                             (commit not yet written)
                                          4. X is not reachable (no commit
                                             references it yet) -> GC deletes X
  5. commit_serialize() -> references X     
  6. object_write(OBJ_COMMIT, ...) -> C     
  7. head_update() -> main now points to C  
  8. Next pes log: reads C, reads tree,     
     tries object_read(X) -> FAILS          
     (X was deleted by GC in step 4)        
```

**Why this is dangerous:**  
The blob `X` is written before the commit that references it (atomic commit ordering). GC runs between those two writes and sees `X` as unreachable because no branch or commit yet points to it. After GC deletes `X`, the commit `C` is written and becomes the branch tip — but its tree references a now-deleted object. The repository is **corrupted**.

**How Git's real GC avoids this:**

1. **Grace period (`--prune=<date>`):** Git's GC only deletes objects older than 2 weeks (default) by comparing the object file's `mtime`. A freshly written blob has `mtime = now`, so it is never pruned during a concurrent operation. `commit_create` writes the blob before the commit, so by the time GC could observe them both, they're both "old." This is Git's primary defense.

2. **`gc.lock` file:** Git creates `.git/gc.pid` before running GC. A concurrent `git commit` checks for this lock and waits or aborts. Only one GC runs at a time.

3. **POSIX `rename()` atomicity:** The `head_update()` rename is atomic — GC either sees the old HEAD or the new HEAD, never a partial state. Combined with the grace period, this ensures GC never races with a live commit operation.

4. **Loose object `mtime` check:** Even without a grace period, Git's `pack-refs` and `repack` phases don't delete loose objects — they're only removed by `git prune` which respects the `--expire` cutoff date.

---

## Summary

| Phase | Functions Implemented | Tests |
|---|---|---|
| 1 — Object Storage | `object_write`, `object_read` | All Phase 1 tests pass |
| 2 — Tree Objects | `tree_from_index` (+ recursive helper) | All Phase 2 tests pass |
| 3 — Index/Staging | `index_load`, `index_save`, `index_add` | `pes add` + `pes status` verified |
| 4 — Commits | `commit_create` | Full integration test passes |

**GitHub Repository:** https://github.com/aag-1/PES2UG24CS018-pes-vcs  
**Total Commits:** 20 (5+ per phase with granular OS-concept commit messages)
