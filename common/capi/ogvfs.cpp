// opengothic-rs: Rust VFS provider (game/capi/og_vfs.h, task 3.4.2). Lives in the
// OpenGothic library like oghooks.cpp, so Resources never depends on opengothic_core.

#include "ogvfs.h"
#include "../../game/capi/og_vfs.h"

#include <Tempest/Log>
#include <Tempest/TextCodec>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Tempest;

namespace {
  og_vfs_provider   provider = {};
  bool              active   = false;
  // A provider mount whose tree isn't in the file index yet (see ogvfs.h).
  std::atomic<bool> pending  {false};
  std::mutex        rebuildSync;

  // Rebuilds `vfs` from the provider's tree. ZenKit's own node type keeps every lookup
  // (Resources::vdfsIndex().find and friends) unchanged; only the content is Rust's.
  void rebuild(zenkit::Vfs& vfs, const char* after) {
    const og_vfs_node* nodes = nullptr;
    size_t             count = 0;
    if(int32_t rc = provider.tree(provider.user, &nodes, &count); rc!=0)
      throw std::runtime_error(std::string("og_vfs: tree() failed after ") + after + ": " + std::to_string(rc));

    auto start = std::chrono::steady_clock::now();
    vfs = zenkit::Vfs();
    // mkdir of the empty path is the root itself; filling it in place avoids copying the
    // whole tree through Vfs::mount.
    zenkit::VfsNode& root = vfs.mkdir("");
    std::vector<zenkit::VfsNode*> dirs(count, nullptr);
    for(size_t i=0; i<count; ++i) {
      auto& n = nodes[i];
      zenkit::VfsNode* parent = &root;
      if(n.parent!=OG_VFS_ROOT) {
        if(n.parent>=i || dirs[n.parent]==nullptr)
          throw std::runtime_error("og_vfs: node " + std::to_string(i) + " has no directory before it as parent");
        parent = dirs[n.parent];
        }
      std::string_view name(n.name, n.name_len);
      size_t before = parent->children().size();
      if(n.is_dir) {
        dirs[i] = parent->create(zenkit::VfsNode::directory(name, n.time));
        } else {
        // del=false: the provider owns the bytes until release().
        auto fd = zenkit::VfsFileDescriptor(reinterpret_cast<const std::byte*>(n.data), n.size, false);
        parent->create(zenkit::VfsNode::file(name, fd, n.time));
        }
      // ZenKit's create replaces a sibling equal after trimming; the provider's tree has
      // none of those, so anything else means the two trees disagree.
      if(parent->children().size()!=before+1)
        throw std::runtime_error("og_vfs: node " + std::to_string(i) + " (" + std::string(name) +
                                 ") replaced a sibling in ZenKit's tree");
      }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    Log::i("og_vfs: file index rebuilt from the Rust VFS after ", after, ": ", count, " nodes in ", int64_t(ms), " ms");
    }
  }

extern "C" void og_vfs_set_provider(const og_vfs_provider* p) {
  if(p==nullptr) {
    provider = {};
    active   = false;
    return;
    }
  provider = *p;
  active   = provider.mount_vdfs && provider.mount_work && provider.tree && provider.release;
  if(!active)
    Log::e("og_vfs: provider with a missing callback ignored, ZenKit's VFS stays");
  }

namespace OgVfs {
  bool mountVdfs(zenkit::Vfs& vfs, const std::vector<std::u16string>& archives) {
    if(!active)
      return false;
    std::vector<std::string> utf8;
    utf8.reserve(archives.size());
    for(auto& a:archives)
      utf8.push_back(TextCodec::toUtf8(a));
    std::vector<const char*> paths;
    paths.reserve(utf8.size());
    for(auto& a:utf8)
      paths.push_back(a.c_str());
    std::lock_guard<std::mutex> g(rebuildSync);
    if(int32_t rc = provider.mount_vdfs(provider.user, paths.data(), paths.size()); rc!=0)
      throw std::runtime_error("og_vfs: mount_vdfs failed: " + std::to_string(rc));
    // No stale ZenKit content in between; the tree follows with mountWork or sync.
    vfs = zenkit::Vfs();
    pending = true;
    return true;
    }

  bool mountWork(zenkit::Vfs& vfs, const std::filesystem::path& path) {
    if(!active)
      return false;
    std::lock_guard<std::mutex> g(rebuildSync);
    if(int32_t rc = provider.mount_work(provider.user, path.string().c_str()); rc!=0)
      throw std::runtime_error("og_vfs: mount_work failed: " + std::to_string(rc));
    rebuild(vfs, "mountWork");
    pending = false;
    return true;
    }

  void sync(zenkit::Vfs& vfs) {
    if(!pending)
      return;
    std::lock_guard<std::mutex> g(rebuildSync);
    if(!pending)
      return;
    rebuild(vfs, "a read of the file index");
    pending = false;
    }

  void release() {
    if(!active)
      return;
    std::lock_guard<std::mutex> g(rebuildSync);
    pending = false;
    provider.release(provider.user);
    }
  }
