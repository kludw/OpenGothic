#pragma once

#include <Tempest/SoundEffect>
#include <Tempest/Sound>
#include <vector>

#include <zenkit/addon/daedalus.hh>

class GSoundEffect;

class SoundFx {
  public:
    SoundFx(std::string_view tagname);
    SoundFx(Tempest::Sound &&raw);
    SoundFx(SoundFx&&)=default;
    SoundFx& operator=(SoundFx&&)=default;

    // variant: a random draw picking one of the sound's variants (opengothic-rs: from OgRng's sound stream)
    Tempest::SoundEffect load(Tempest::SoundDevice& dev, bool& loop, uint32_t variant) const;

  private:
    struct SoundVar {
      SoundVar()=default;
      SoundVar(const zenkit::ISoundEffect& sfx,Tempest::Sound&& snd);
      SoundVar(const float vol,Tempest::Sound&& snd);

      Tempest::Sound snd;
      float          vol  = 0.5f;
      bool           loop = false;
      };

    std::vector<SoundVar> inst;
    void implLoad    (std::string_view name);
    void loadVariants(std::string_view name);
  };

