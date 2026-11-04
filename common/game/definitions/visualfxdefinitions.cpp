#include "visualfxdefinitions.h"

#include <Tempest/Log>

#include "graphics/visualfx.h"
#include "gothic.h"

using namespace Tempest;

VisualFxDefinitions::VisualFxDefinitions() {
  vm = Gothic::inst().createPhoenixVm("VisualFx.dat");
  // opengothic-rs (task 4.15): dump every effect and every emit key (in play VisualFx's
  // constructor inits the keys an effect names); the instances aren't kept.
  if(vm && OgVm::dumping()) {
    vm->enumerate_instances_by_class_name("CFX_BASE", [this](zenkit::DaedalusSymbol& s) {
      implGet(s.name());
      });
    vm->enumerate_instances_by_class_name("C_PARTICLEFXEMITKEY", [this](zenkit::DaedalusSymbol& s) {
      vm->init_instance<zenkit::IParticleEffectEmitKey>(&s);
      });
    }
  }

VisualFxDefinitions::~VisualFxDefinitions() {
  }

const VisualFx* VisualFxDefinitions::get(std::string_view name) {
  std::string cname = std::string(name);
  auto it = vfx.find(cname);
  if(it!=vfx.end())
    return it->second.get();

  auto def = implGet(cname);
  if(def == nullptr)
    return nullptr;

  auto ret = vfx.insert(std::make_pair<std::string,std::unique_ptr<VisualFx>>(std::move(cname),nullptr));
  ret.first->second.reset(new VisualFx(*def,*vm,name));

  auto& vfx = *ret.first->second;
  vfx.dbgName = name.data();

  return &vfx;
  }

std::shared_ptr<zenkit::IEffectBase> VisualFxDefinitions::implGet(std::string_view name) {
  if(!vm || name.empty())
    return nullptr;

  auto id = vm->find_symbol_by_name(name);
  if(id==nullptr) {
    Log::e("invalid visual effect: \"",name,"\"");
    return nullptr;
    }

  try {
    return vm->init_instance<zenkit::IEffectBase>(id);
    }
  catch (const zenkit::DaedalusScriptError&) {
    return nullptr;
    }
  }
