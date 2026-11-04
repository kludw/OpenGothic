#pragma once

#include "script/ogvm.h"
#include <zenkit/addon/daedalus.hh>

#include <memory>

class MusicDefinitions final {
  public:
    MusicDefinitions();
    ~MusicDefinitions();

    const zenkit::IMusicTheme* operator[](std::string_view name) const;

  private:
    std::unique_ptr<OgVm>  vm;
    std::vector<std::shared_ptr<zenkit::IMusicTheme>> themes;
  };
