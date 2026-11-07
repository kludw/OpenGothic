#include "serialize.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>

#include "savegameheader.h"
#include "world/world.h"
#include "world/fplock.h"
#include "world/waypoint.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <Tempest/Log>
#include <Tempest/Application>

size_t Serialize::writeFunc(void* pOpaque, uint64_t file_ofs, const void* pBuf, size_t n) {
  auto& self = *reinterpret_cast<Serialize*>(pOpaque);
  file_ofs += mz_zip_get_archive_file_start_offset(&self.impl);
  if(file_ofs!=self.curOffset) {
    self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
    return 0;
    }
  size_t ret = self.fout->write(pBuf,n);
  self.curOffset+=ret;
  return ret;
  }

size_t Serialize::readFunc(void* pOpaque, uint64_t file_ofs, void* pBuf, size_t n) {
  auto& self = *reinterpret_cast<Serialize*>(pOpaque);
  file_ofs += mz_zip_get_archive_file_start_offset(&self.impl);
  if(self.curOffset<file_ofs) {
    size_t diff = size_t(file_ofs-self.curOffset);
    size_t mv   = self.fin->seek(diff);
    self.curOffset += uint64_t(mv);
    if(diff!=mv) {
      self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
      return 0;
      }
    }
  if(self.curOffset>file_ofs) {
    size_t diff = size_t(self.curOffset-file_ofs);
    size_t mv   = self.fin->unget(diff);
    self.curOffset -= uint64_t(mv);
    if(diff!=mv) {
      self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
      return 0;
      }
    }
  size_t ret = self.fin->read(pBuf,n);
  self.curOffset+=ret;
  return ret;
  }

Serialize::Serialize() {}

//static uint64_t time0 = 0;

Serialize::Serialize(Tempest::ODevice& fout) : fout(&fout) {
  //time0 = Tempest::Application::tickCount();
  entryBuf .reserve(1*1024*1024);
  entryName.reserve(256);

  impl.m_pWrite           = Serialize::writeFunc;
  impl.m_pIO_opaque       = this;
  impl.m_zip_type         = MZ_ZIP_TYPE_USER;
  if(!mz_zip_writer_init_v2(&impl, 0, 0))
    throw std::runtime_error("unable to create game archive");
  }

Serialize::Serialize(Tempest::IDevice& fin) : fin(&fin) {
  entryBuf .resize(1*1024*1024);
  entryName.reserve(256);

  impl.m_pRead            = Serialize::readFunc;
  impl.m_pIO_opaque       = this;
  impl.m_zip_type         = MZ_ZIP_TYPE_USER;
  if(!mz_zip_reader_init(&impl, fin.size(), 0))
    throw std::runtime_error("unable to read game archive");
  }

Serialize::Serialize(std::string& json) : dump(&json) {
  }

Serialize::~Serialize() noexcept(false) {
  if(dump!=nullptr && dumpEntry)
    dump->append("]]");
  if(fin!=nullptr)
    mz_zip_reader_end(&impl);
  if(fout==nullptr)
    return;
  try {
    closeEntry();
    if(!mz_zip_writer_finalize_archive(&impl) || !fout->flush())
      throw std::runtime_error("unable to finalize game archive");
    }
  catch(...) {
    mz_zip_writer_end(&impl);
    if(std::uncaught_exceptions()==0)
      throw;
    return;
    }
  mz_zip_writer_end(&impl);
  }

std::string_view Serialize::worldName() const {
  if(ctx!=nullptr)
    return ctx->name();
  return "_";
  }

void Serialize::closeEntry() {
  if(fout==nullptr)
    return;
  if(entryBuf.empty())
    return;

  mz_uint level  = entryBuf.size()>256 ? MZ_BEST_SPEED : MZ_NO_COMPRESSION;
  mz_bool status = mz_zip_writer_add_mem(&impl, entryName.c_str(), entryBuf.data(), entryBuf.size(), level);
  entryBuf .clear();
  entryName.clear();
  if(!status)
    throw std::runtime_error("unable to write entry in game archive");
  }

bool Serialize::implSetEntry(std::string_view fname) {
  if(dump!=nullptr) {
    if(dumpEntry)
      dump->append("]],\n");
    dump->push_back('[');
    jsonString(*dump,fname);
    dump->append(",[");
    dumpEntry = true;
    dumpFirst = true;
    return true;
    }
  size_t prefix = 0;
  if(fout!=nullptr) {
    while(prefix<fname.size() && prefix<entryName.size()) {
      if(entryName[prefix]!=fname[prefix])
        break;
      ++prefix;
      }
    }
  closeEntry();
  entryName = fname;
  if(fout!=nullptr) {
    for(size_t i=prefix; i<entryName.size(); ++i) {
      if(entryName[i]=='/' && i+1<entryName.size()) {
        const char prev = entryName[i+1];
        entryName[i+1] = '\0';
        const auto it = outFileList.insert(entryName.c_str());
        if(it.second) {
          mz_bool status = mz_zip_writer_add_mem(&impl, entryName.c_str(), NULL, 0, MZ_NO_COMPRESSION);
          if(!status)
            throw std::runtime_error("unable to allocate entry in game archive");
          }
        entryName[i+1] = prev;
        }
      }
    return true;
    }
  if(fin!=nullptr) {
    mz_uint32 id = mz_uint32(-1);
    if(mz_zip_reader_locate_file_v2(&impl, entryName.c_str(), nullptr, 0, &id)) {
      mz_zip_archive_file_stat stat = {};
      mz_zip_reader_file_stat(&impl,id,&stat);
      entryBuf.resize(size_t(stat.m_uncomp_size));
      mz_zip_reader_extract_file_to_mem(&impl,entryName.c_str(),entryBuf.data(),entryBuf.size(),0);
      } else {
      entryBuf.clear();
      }
    readOffset = 0;
    return !entryBuf.empty();
    }
  return false;
  }

uint32_t Serialize::implDirectorySize(std::string_view e) {
  uint32_t count = 0;
  std::vector<char> filename;
  filename.reserve(256);
  for(mz_uint i=0; i<mz_zip_reader_get_num_files(&impl); ++i) {
    const auto length = mz_zip_reader_get_filename(&impl, i, nullptr, 0);
    if(length==0)
      throw std::runtime_error("unable to locate entry in game archive");
    filename.resize(length);
    if(mz_zip_reader_get_filename(&impl, i, filename.data(), length)!=length)
      throw std::runtime_error("unable to read entry name in game archive");
    std::string_view name(filename.data(), length-1);
    if(name.size()<=e.size() || !name.starts_with(e))
      continue;
    const auto child = name.substr(e.size());
    const auto separator = child.find('/');
    if(separator==std::string_view::npos || separator+1==child.size())
      ++count;
    }
  return count;
  }

void Serialize::writeBytes(const void* buf, size_t sz) {
  if(dump!=nullptr) {
    dumpHex(buf,sz);
    return;
    }
  if(sz==0)
    return;
  size_t at = entryBuf.size();
  entryBuf.resize(entryBuf.size()+sz);
  std::memcpy(&entryBuf[at],buf,sz);
  }

void Serialize::readBytes(void* buf, size_t sz) {
  if(fin==nullptr || readOffset+sz>entryBuf.size())
    throw std::runtime_error("unable to read save-game file");
  std::memcpy(buf,&entryBuf[size_t(readOffset)],sz);
  readOffset+=sz;
  }

void Serialize::implWrite(const std::string& s) {
  if(dump!=nullptr) {
    implWrite(std::string_view(s));
    return;
    }
  uint32_t sz=uint32_t(s.size());
  implWrite(sz);
  writeBytes(s.data(),sz);
  }

void Serialize::implRead(std::string& s) {
  uint32_t sz=0;
  implRead(sz);
  s.resize(sz);
  if(sz>0)
    readBytes(&s[0],sz);
  }

void Serialize::implWrite(std::string_view s) {
  if(dump!=nullptr) {
    std::string v;
    jsonString(v,s);
    dumpRaw(v);
    return;
    }
  uint32_t sz=uint32_t(s.size());
  implWrite(sz);
  writeBytes(s.data(),sz);
  }

void Serialize::implWrite(WeaponState w) {
  implWrite(uint8_t(w));
  }

void Serialize::implRead(WeaponState &w) {
  implRead(reinterpret_cast<uint8_t&>(w));
  }

void Serialize::implWrite(const WayPoint* wptr) {
  implWrite(wptr ? wptr->name : "");
  }

void Serialize::implRead(const WayPoint*& wptr) {
  implRead(tmpStr);
  wptr = ctx==nullptr ? nullptr : ctx->findPoint(tmpStr,false);
  }

void Serialize::implWrite(const ScriptFn& fn) {
  uint32_t v = uint32_t(-1);
  if(fn.ptr<uint64_t(std::numeric_limits<uint32_t>::max()))
    v = uint32_t(fn.ptr);
  implWrite(v);
  }

void Serialize::implRead(ScriptFn& fn) {
  uint32_t v=0;
  implRead(v);
  if(v==std::numeric_limits<uint32_t>::max())
    fn.ptr = size_t(-1); else
    fn.ptr = v;
  }

void Serialize::implWrite(const Npc* npc) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->npcId(npc);
  implWrite(id);
  }

void Serialize::implRead(const Npc *&npc) {
  uint32_t id=uint32_t(-1);
  implRead(id);
  npc = ctx==nullptr ? nullptr : ctx->npcById(id);
  }

void Serialize::implWrite(Npc *npc) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->npcId(npc);
  implWrite(id);
  }

void Serialize::implRead(Npc *&npc) {
  uint32_t id=uint32_t(-1);
  read(id);
  npc = ctx==nullptr ? nullptr : ctx->npcById(id);
  }

void Serialize::implWrite(Interactive* mobsi) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->mobsiId(mobsi);
  implWrite(id);
  }

void Serialize::implRead(Interactive*& mobsi) {
  uint32_t id=uint32_t(-1);
  implRead(id);
  mobsi = ctx==nullptr ? nullptr : ctx->mobsiById(id);
  }

void Serialize::implWrite(const SaveGameHeader& p) {
  p.save(*this);
  }

void Serialize::implRead(SaveGameHeader& p) {
  p = SaveGameHeader(*this);
  }

void Serialize::implWrite(const Tempest::Pixmap& p) {
  implWrite(p, "png");
  }

void Serialize::implWrite(const Tempest::Pixmap& p, const char* ext) {
  if(dump!=nullptr) {
    // The preview screenshot is render-only: its size stands for it.
    dumpNum(uint64_t(p.w()));
    dumpNum(uint64_t(p.h()));
    return;
    }
  std::vector<uint8_t> tmp;
  tmp.reserve(4*1024*1024);
  Tempest::MemWriter w{tmp};
  p.save(w, ext);
  writeBytes(tmp.data(),tmp.size());
  }

void Serialize::implRead(Tempest::Pixmap& p) {
  Tempest::MemReader r{&entryBuf[size_t(readOffset)],size_t(entryBuf.size()-readOffset)};
  p = Tempest::Pixmap(r);
  readOffset += r.cursorPosition();
  }

void Serialize::implWrite(const zenkit::INpc& h) {
  write(uint32_t(h.symbol_index()));
  write(h.id,h.name,h.slot,h.effect,int32_t(h.type));
  write(int32_t(h.flags));
  write(h.attribute,h.hitchance,h.protection,h.damage);
  write(h.damage_type,h.guild,h.level);
  write(h.mission);
  write(h.fight_tactic,h.weapon,h.voice,h.voice_pitch,h.body_mass);
  write(h.daily_routine,h.start_aistate);
  write(h.spawnpoint,h.spawn_delay,h.senses,h.senses_range);
  write(h.aivar);
  write(h.wp,h.exp,h.exp_next,h.lp,h.bodystate_interruptable_override,h.no_focus);
  }

void Serialize::readNpc(OgVm& vm, std::shared_ptr<zenkit::INpc>& h) {
  uint32_t instanceSymbol=0;
  read(instanceSymbol);

  auto sym = vm.find_symbol_by_index(instanceSymbol);

  if (sym != nullptr) {
    vm.allocate_instance(h, sym);
    } else {
    Tempest::Log::e("Cannot load serialized NPC ", instanceSymbol, ": Symbol not found.");
    }

  read(h->id,h->name,h->slot,h->effect, reinterpret_cast<int32_t&>(h->type));
  read(reinterpret_cast<int32_t&>(h->flags));
  read(h->attribute,h->hitchance,h->protection,h->damage);
  read(h->damage_type,h->guild,h->level);
  read(h->mission);
  read(h->fight_tactic,h->weapon,h->voice,h->voice_pitch,h->body_mass);
  read(h->daily_routine,h->start_aistate);
  read(h->spawnpoint,h->spawn_delay,h->senses,h->senses_range);
  read(h->aivar);
  read(h->wp,h->exp,h->exp_next,h->lp,h->bodystate_interruptable_override,h->no_focus);
  }

void Serialize::implWrite(const FpLock &fp) {
  fp.save(*this);
  }

void Serialize::implRead(FpLock &fp) {
  fp.load(*this);
  }

void Serialize::implWrite(const zenkit::AnimationSample& i) {
  if(dump!=nullptr) {
    dumpFloats(&i,sizeof(i));
    return;
    }
  writeBytes(&i,sizeof(i));
  }

void Serialize::implRead(zenkit::AnimationSample& i) {
  if (version() < 40) {
    read(i.position.x,i.position.y,i.position.z);
    read(i.rotation.x,i.rotation.y,i.rotation.z,i.rotation.w);
    } else {
    readBytes (&i,sizeof(i));
    }
  }

// opengothic-rs (task 5.18): dump mode. Values are separated by commas inside the open
// entry; a value before the first setEntry goes into an entry named "".
void Serialize::dumpRaw(std::string_view token) {
  if(!dumpEntry)
    implSetEntry("");
  if(!dumpFirst)
    dump->push_back(',');
  dumpFirst = false;
  dump->append(token);
  }

void Serialize::dumpNum(int64_t v) {
  char buf[32] = {};
  std::snprintf(buf,sizeof(buf),"%lld",static_cast<long long>(v));
  dumpRaw(buf);
  }

void Serialize::dumpNum(uint64_t v) {
  char buf[32] = {};
  std::snprintf(buf,sizeof(buf),"%llu",static_cast<unsigned long long>(v));
  dumpRaw(buf);
  }

void Serialize::dumpFloat(float f) {
  std::string v;
  jsonFloat(v,f);
  dumpRaw(v);
  }

void Serialize::dumpFloats(const void* v, size_t sz) {
  if(sz%sizeof(float)!=0) {
    dumpHex(v,sz);
    return;
    }
  std::string out = "[";
  for(size_t i=0; i<sz/sizeof(float); ++i) {
    float f = 0;
    std::memcpy(&f,static_cast<const uint8_t*>(v)+i*sizeof(float),sizeof(f));
    if(i>0)
      out.push_back(',');
    jsonFloat(out,f);
    }
  out.push_back(']');
  dumpRaw(out);
  }

void Serialize::dumpHex(const void* v, size_t sz) {
  static const char digits[] = "0123456789abcdef";
  std::string out = "\"x:";
  for(size_t i=0; i<sz; ++i) {
    const uint8_t b = static_cast<const uint8_t*>(v)[i];
    out.push_back(digits[b>>4]);
    out.push_back(digits[b&0xF]);
    }
  out.push_back('"');
  dumpRaw(out);
  }

// Game strings are code-page bytes, not UTF-8: bytes >= 0x80 are written as \u00XX
// (their Latin-1 reading), so the dump is valid JSON and every byte survives.
void Serialize::jsonString(std::string& out, std::string_view s) {
  static const char digits[] = "0123456789abcdef";
  out.push_back('"');
  for(const char c:s) {
    const auto b = uint8_t(c);
    if(b=='"' || b=='\\') {
      out.push_back('\\');
      out.push_back(char(b));
      }
    else if(b<0x20 || b>=0x7F) {
      out.append("\\u00");
      out.push_back(digits[b>>4]);
      out.push_back(digits[b&0xF]);
      }
    else {
      out.push_back(char(b));
      }
    }
  out.push_back('"');
  }

// %.9g gives back the same float; NaN and infinities are no JSON numbers.
void Serialize::jsonFloat(std::string& out, float f) {
  if(std::isnan(f)) {
    out.append("\"nan\"");
    return;
    }
  if(std::isinf(f)) {
    out.append(f>0 ? "\"inf\"" : "\"-inf\"");
    return;
    }
  char buf[32] = {};
  std::snprintf(buf,sizeof(buf),"%.9g",double(f));
  out.append(buf);
  }
