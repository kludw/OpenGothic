/* opengothic-rs: Rust VFS provider (task 3.4.2), lane B's seam file next to og_capi.h.
 *
 * With a provider registered, Resources::mountVdfs and Resources::mountWork hand their
 * arguments to the provider instead of mounting with ZenKit, and the game's file index
 * (Resources::vdfsIndex) is rebuilt from the provider's tree after each mount. Lookups
 * stay ZenKit's (same tree, same order); which files exist, which archive wins and the
 * bytes behind every file come from the provider.
 *
 * Plain C ABI. Callbacks must not throw or unwind (a Rust host wraps them in
 * catch_unwind). They run one at a time, under a mutex of the fork: the mounts on the
 * thread that starts the game, a deferred rebuild (common/capi/ogvfs.h) on whichever
 * thread first reads the file index. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_VFS_ROOT UINT32_MAX

/* One node of the provider's tree. Parents come before their children; `parent` is the
 * index of the parent directory, OG_VFS_ROOT for children of the root. */
typedef struct og_vfs_node {
  const char*    name;      /* raw bytes, not NUL-terminated */
  size_t         name_len;
  int64_t        time;      /* Unix seconds, as ZenKit's VfsNode::time */
  uint32_t       parent;
  uint8_t        is_dir;
  const uint8_t* data;      /* files: provider-owned bytes, valid until release */
  size_t         size;
  } og_vfs_node;

typedef struct og_vfs_provider {
  void* user;
  /* Mounts the archives OpenGothic found (UTF-8 paths, detection order; the provider
   * sorts them as the game does). 0 on success; on failure the game logs and stops. */
  int32_t (*mount_vdfs)(void* user, const char* const* paths, size_t count);
  /* Mounts the host _work directory below /_work (UTF-8 path). 0 on success. */
  int32_t (*mount_work)(void* user, const char* path);
  /* The tree after the last mount: `*count` nodes at `*nodes`, valid until the next
   * call into the provider. 0 on success. */
  int32_t (*tree)(void* user, const og_vfs_node** nodes, size_t* count);
  /* Resources::unloadAll: forget the tree; bytes handed out before are invalid after. */
  void (*release)(void* user);
  } og_vfs_provider;

/* Registers the provider (copied) before og_init/og_run, once per process; NULL keeps
 * ZenKit's VFS. `user` must stay valid until the game is gone. Every callback must be
 * set. */
void og_vfs_set_provider(const og_vfs_provider* provider);

#ifdef __cplusplus
}
#endif
