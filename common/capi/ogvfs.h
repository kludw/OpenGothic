#pragma once

// opengothic-rs: hooks of the Rust VFS provider (game/capi/og_vfs.h) for Resources.

#include <zenkit/Vfs.hh>

#include <filesystem>
#include <string>
#include <vector>

namespace OgVfs {
  // With a provider: mount through it and rebuild `vfs` from its tree, true. Without:
  // false, Resources mounts with ZenKit as stock. Throws if the provider fails.
  // mountVdfs only mounts in Rust and leaves `vfs` empty until mountWork (the game calls
  // both back to back) or the next sync: rebuilding costs ~0.2 s per 30k nodes.
  bool mountVdfs(zenkit::Vfs& vfs, const std::vector<std::u16string>& archives);
  bool mountWork(zenkit::Vfs& vfs, const std::filesystem::path& path);
  // Before every read of the file index: rebuilds it if a mount is still pending.
  void sync(zenkit::Vfs& vfs);
  // Resources::unloadAll, after the file index is gone.
  void release();
  }
