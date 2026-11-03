#pragma once

// opengothic-rs: hooks of the Rust file-format provider (game/capi/og_formats.h) for
// Resources.

#include "../../game/capi/og_formats.h"

#include <zenkit/Font.hh>
#include <zenkit/Vfs.hh>

#include <chrono>
#include <memory>
#include <string_view>

namespace OgFormats {
  // A texture decoded by the provider; gives the bytes back when destroyed.
  class Texture {
    public:
      explicit Texture(const og_formats_texture& raw):raw(raw){}
      Texture(const Texture&) = delete;
      Texture& operator=(const Texture&) = delete;
      ~Texture();

      bool           isDds()  const { return raw.dds!=0; }
      uint32_t       width()  const { return raw.width;  }
      uint32_t       height() const { return raw.height; }
      const uint8_t* data()   const { return raw.data;   }
      size_t         size()   const { return raw.size;   }

    private:
      og_formats_texture raw; // handle==nullptr: nothing to release
      friend std::unique_ptr<Texture> loadTexture(const zenkit::VfsNode&, std::string_view);
    };

  // With a provider: the ZTEX file `entry` decoded in Rust. Without: nullptr, Resources
  // decodes with ZenKit as stock. Throws if the provider fails (where ZenKit would).
  std::unique_ptr<Texture> loadTexture(const zenkit::VfsNode& entry, std::string_view name);

  // With a provider: fills `font` from the FNT file `data`, decoded in Rust, true. Without:
  // false, GthFont loads with ZenKit as stock. Throws if the provider fails.
  bool loadFont(zenkit::Read& data, std::string_view name, zenkit::Font& font);

  // Times one `-C.TEX` load in either mode (decode and upload), for the "no slower than
  // C++" check: the totals are logged every 256 textures.
  class TextureTimer {
    public:
      TextureTimer();
      ~TextureTimer();
    private:
      std::chrono::steady_clock::time_point start;
    };
  }
