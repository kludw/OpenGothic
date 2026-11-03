/* opengothic-rs: Rust file-format provider (task 3.4.3), lane B's seam file next to
 * og_capi.h and og_vfs.h.
 *
 * With a provider registered, loaders hand the bytes of a game file to the provider
 * instead of parsing them with ZenKit, and get back what OpenGothic used to compute from
 * ZenKit's result. Without one, every loader is stock.
 *
 * Swapped so far: ZenKit textures (`*-C.TEX`, Resources::implLoadTextureUncached) and
 * bitmap fonts (`*.FNT`, GthFont).
 *
 * Plain C ABI. Callbacks must not throw or unwind (a Rust host wraps them in
 * catch_unwind). They may run on any thread and at the same time; results are
 * independent and owned by the provider until released. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the game uploads for one texture: DXT1-5 as a DDS file (zenkit::to_dds), every
 * other format as the RGBA8 pixels of mipmap 0 (zenkit::Texture::as_rgba8(0)). */
typedef struct og_formats_texture {
  uint8_t        dds;     /* 1: `data` is a DDS file; 0: `width * height` RGBA8 pixels */
  uint32_t       width;   /* RGBA8 only; 0 for DDS (the file has its own header) */
  uint32_t       height;
  const uint8_t* data;    /* provider-owned, valid until release_texture */
  size_t         size;
  void*          handle;  /* the provider's (never NULL), passed back to release_texture */
  } og_formats_texture;

/* One glyph of a bitmap font, as zenkit::FontGlyph. */
typedef struct og_formats_glyph {
  uint8_t width;   /* advance in pixels */
  float   uv[4];   /* top-left x, y, bottom-right x, y, in fractions of the font texture */
  } og_formats_glyph;

/* A bitmap font (zenkit::Font). */
typedef struct og_formats_font {
  const char*             name;      /* raw bytes, not NUL-terminated */
  size_t                  name_len;
  uint32_t                height;
  const og_formats_glyph* glyphs;    /* provider-owned, valid until release_font */
  size_t                  glyph_count;
  void*                   handle;    /* the provider's, passed back to release_font */
  } og_formats_font;

typedef struct og_formats_provider {
  void* user;
  /* Decodes the ZTEX file `bytes` (`size` bytes) into `*out`. `name` is for the
   * provider's log (raw bytes, not NUL-terminated). Either pointer may be NULL when its
   * length is 0. 0 on success; otherwise the file is broken (the provider logs why) and
   * the game fails where ZenKit would have thrown. */
  int32_t (*texture)(void* user, const char* name, size_t name_len,
                     const uint8_t* bytes, size_t size, og_formats_texture* out);
  /* Frees what `texture` handed out in `*t` (only after it returned 0). */
  void (*release_texture)(void* user, const og_formats_texture* t);
  /* Decodes the FNT file `bytes` into `*out` (`name`: the font's texture, for the log).
   * 0 on success; otherwise broken, as for `texture`. */
  int32_t (*font)(void* user, const char* name, size_t name_len,
                  const uint8_t* bytes, size_t size, og_formats_font* out);
  /* Frees what `font` handed out in `*f` (only after it returned 0). */
  void (*release_font)(void* user, const og_formats_font* f);
  } og_formats_provider;

/* Registers the provider (copied) before og_init/og_run, once per process; NULL keeps
 * ZenKit's parsers. `user` must stay valid until the game is gone. Every callback must be
 * set. */
void og_formats_set_provider(const og_formats_provider* provider);

#ifdef __cplusplus
}
#endif
