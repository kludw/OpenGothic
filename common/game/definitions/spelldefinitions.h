#pragma once

#include "script/ogvm.h"
#include <zenkit/addon/daedalus.hh>

class SpellDefinitions final {
  public:
    SpellDefinitions(OgVm &vm);
    ~SpellDefinitions();

    const zenkit::ISpell& find(std::string_view instanceName) const;

  private:
    OgVm&                          vm;
    std::vector<std::shared_ptr<zenkit::ISpell>> spl;
  };
