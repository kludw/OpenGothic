#include "ogvm.h"

OgVm::OgVm(zenkit::DaedalusScript&& script, uint8_t flags, std::string name)
  : zk(std::make_unique<Zk>(std::move(script), flags)), vmName(std::move(name)) {
  ScriptTrace::vm(*zk, vmName, zk->symbols().size());
  }

OgVm::OgVm(OgVm&&) noexcept = default;

OgVm& OgVm::operator=(OgVm&&) noexcept = default;

OgVm::~OgVm() = default;

const OgVm::External* OgVm::external(std::string_view name) const {
  auto* sym = zk->find_symbol_by_name(name);
  if(sym==nullptr)
    return nullptr;
  auto it = externals.find(sym->name());
  if(it==externals.end())
    return nullptr;
  return &it->second;
  }
