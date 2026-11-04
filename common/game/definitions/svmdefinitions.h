#pragma once

#include "script/ogvm.h"
#include <zenkit/addon/daedalus.hh>

#include <memory>

class SvmDefinitions final {
  public:
    SvmDefinitions(OgVm& vm);
    ~SvmDefinitions();

    std::string_view find(std::string_view speech, int id);

  private:
    OgVm&                                    vm;
    std::vector<std::shared_ptr<zenkit::DaedalusInstance>> svm;
  };

