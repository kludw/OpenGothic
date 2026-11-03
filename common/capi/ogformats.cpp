// opengothic-rs: Rust file-format provider (game/capi/og_formats.h, task 3.4.3). Lives
// in the OpenGothic library like ogvfs.cpp, so Resources never depends on opengothic_core.

#include "ogformats.h"

#include <Tempest/Log>

#include <zenkit/Stream.hh>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Tempest;

namespace {
  og_formats_provider   provider = {};
  bool                  active   = false;
  std::atomic<uint64_t> texCount {0};
  std::atomic<uint64_t> texNanos {0};
  }

extern "C" void og_formats_set_provider(const og_formats_provider* p) {
  if(p==nullptr) {
    provider = {};
    active   = false;
    return;
    }
  provider = *p;
  active   = provider.texture && provider.release_texture && provider.font && provider.release_font;
  if(!active)
    Log::e("og_formats: provider with a missing callback ignored, ZenKit's parsers stay");
  }

namespace OgFormats {
  Texture::~Texture() {
    if(raw.handle!=nullptr)
      provider.release_texture(provider.user, &raw);
    }

  static std::vector<uint8_t> readAll(zenkit::Read& reader) {
    std::vector<uint8_t> bytes;
    reader.seek(0, zenkit::Whence::END);
    bytes.resize(reader.tell());
    reader.seek(0, zenkit::Whence::BEG);
    reader.read(bytes.data(), bytes.size());
    return bytes;
    }

  std::unique_ptr<Texture> loadTexture(const zenkit::VfsNode& entry, std::string_view name) {
    if(!active)
      return nullptr;
    auto reader = entry.open_read();
    auto bytes  = readAll(*reader);

    // Allocated first: once the provider handed out a buffer, nothing may throw before
    // a Texture owns it.
    auto tex = std::make_unique<Texture>(og_formats_texture{});
    if(int32_t rc = provider.texture(provider.user, name.data(), name.size(), bytes.data(), bytes.size(), &tex->raw); rc!=0) {
      tex->raw = {};
      throw std::runtime_error("og_formats: texture " + std::string(name) + " failed: " + std::to_string(rc));
      }
    size_t rgba = 0;
    if(!tex->isDds() && (__builtin_mul_overflow(size_t(tex->width())*4, size_t(tex->height()), &rgba) || tex->size()!=rgba))
      throw std::runtime_error("og_formats: texture " + std::string(name) + ": " + std::to_string(tex->size()) +
                               " bytes for " + std::to_string(tex->width()) + "x" + std::to_string(tex->height()) + " RGBA8");
    return tex;
    }

  bool loadFont(zenkit::Read& data, std::string_view name, zenkit::Font& font) {
    if(!active)
      return false;
    auto bytes = readAll(data);
    og_formats_font raw = {};
    if(int32_t rc = provider.font(provider.user, name.data(), name.size(), bytes.data(), bytes.size(), &raw); rc!=0)
      throw std::runtime_error("og_formats: font for " + std::string(name) + " failed: " + std::to_string(rc));
    // Released also if copying throws (bad_alloc).
    struct Release {
      og_formats_font& f;
      ~Release() { provider.release_font(provider.user, &f); }
      } release{raw};
    font.name   = std::string(raw.name, raw.name_len);
    font.height = raw.height;
    font.glyphs.resize(raw.glyph_count);
    for(size_t i=0; i<raw.glyph_count; ++i) {
      auto& g = raw.glyphs[i];
      font.glyphs[i].width = g.width;
      font.glyphs[i].uv[0] = zenkit::Vec2(g.uv[0], g.uv[1]);
      font.glyphs[i].uv[1] = zenkit::Vec2(g.uv[2], g.uv[3]);
      }
    return true;
    }

  TextureTimer::TextureTimer():start(std::chrono::steady_clock::now()) {
    }

  TextureTimer::~TextureTimer() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    uint64_t total = texNanos.fetch_add(uint64_t(ns)) + uint64_t(ns);
    uint64_t n     = texCount.fetch_add(1) + 1;
    if(n%256==0)
      Log::i("og_formats: ", n, " textures loaded in ", int64_t(total/1000000), " ms by ", active ? "rust" : "zenkit");
    }
  }
