#pragma once

#include <Tempest/IDevice>
#include <Tempest/ODevice>
#include <Tempest/Pixmap>

#include <Tempest/Matrix4x4>

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <type_traits>
#include <ctime>

#include <miniz.h>

#include "script/ogvm.h"
#include <zenkit/addon/daedalus.hh>
#include <zenkit/ModelAnimation.hh>

#include "gametime.h"
#include "constants.h"
#include "utils/string_frm.h"

class WayPoint;
class Npc;
class Interactive;
class World;
class FpLock;
class ScriptFn;
class SaveGameHeader;

class Serialize {
  public:
    enum Version : uint16_t {
      Current    = 55,
      MinVersion = 36,

      Last_2025  = 53,
      Last_2024  = 49,
      Last_2023  = 46,
      Last_2022  = 41,
      Last_2021  = 36,
      First      = 36, // first zip-based version
      };
    Serialize(Tempest::ODevice& fout);
    Serialize(Tempest::IDevice&  fin);
    // opengothic-rs (task 5.18): dump mode. Every value a save writes is appended to
    // json as canonical JSON instead (og_dump_world); entries become ["name",[values]].
    explicit Serialize(std::string& json);
    Serialize(Serialize&&)=delete;
    ~Serialize() noexcept(false);

    uint16_t version()              const { return wldVer; }
    void     setVersion(uint16_t v)       { wldVer = v;    }
    uint16_t globalVersion()        const { return curVer; }
    void     setGlobalVersion(uint16_t v) { curVer = v;    }

    template<class ... Args>
    bool setEntry(const Args& ... args) {
      string_frm s(args...);
      return implSetEntry(s);
      }

    template<class ... Args>
    uint32_t directorySize(const Args& ... args) {
      string_frm s(args...);
      return implDirectorySize(s);
      }

    void setContext(World* ctx) { this->ctx=ctx; }
    std::string_view worldName() const;

    // raw
    void writeBytes(const void* v,size_t sz);
    void readBytes (void* v,size_t sz);

    template<class ... Arg>
    void write(const Arg& ... a){
      (implWrite(a),... );
      }

    template<class ... Arg>
    void read(Arg& ... a){
      (implRead(a),... );
      }

    void readNpc(OgVm& vm, std::shared_ptr<zenkit::INpc>& npc);

    // opengothic-rs (task 5.18): the JSON spellings the dump uses, also for og_dump_world.
    static void jsonString(std::string& out, std::string_view s);
    static void jsonFloat (std::string& out, float f);

  private:
    Serialize();

    // trivial types
    void implWrite(bool      i) { if(dump!=nullptr) dumpRaw(i ? "true" : "false"); else implWrite(uint8_t(i ? 1 : 0)); }
    void implRead (bool&     i) { uint8_t x=0; read(x); i=(x!=0); }

    void implWrite(char      i) { if(dump!=nullptr) dumpNum(int64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (char&     i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint8_t   i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (uint8_t&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint16_t  i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (uint16_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(int32_t   i) { if(dump!=nullptr) dumpNum(int64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (int32_t&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint32_t  i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (uint32_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint64_t  i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (uint64_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(float  i)    { if(dump!=nullptr) dumpFloat(i); else writeBytes(&i,sizeof(i)); }
    void implRead (float& i)    { readBytes (&i,sizeof(i)); }

    void implWrite(zenkit::DaedalusDataType  i)    { write((uint32_t) i); }
    void implRead (zenkit::DaedalusDataType& i)    { read((uint32_t&) i); }

    template<class T, std::enable_if_t<!std::is_same<T,uint32_t>::value && !std::is_same<T,uint64_t>::value && std::is_same<T,size_t>::value,bool> = true>
    void implWrite(T ) = delete;
    template<class T, std::enable_if_t<!std::is_same<T,uint32_t>::value && !std::is_same<T,uint64_t>::value && std::is_same<T,size_t>::value,bool> = true>
    void implRead(T&) = delete;

    // typedef
    void implWrite(gtime  i)    { if(dump!=nullptr) dumpNum(i.toInt()); else writeBytes(&i,sizeof(i)); }
    void implRead (gtime& i)    { readBytes (&i,sizeof(i)); }

    void implWrite(WalkBit   i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (WalkBit&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(BodyState i) { if(dump!=nullptr) dumpNum(uint64_t(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (BodyState&i) { readBytes (&i,sizeof(i)); }

    void implWrite(Attitude  i) { if(dump!=nullptr) dumpNum(int64_t(i)); else writeBytes(&i,sizeof(i));}
    void implRead (Attitude& i) { readBytes (&i,sizeof(i)); }

    void implWrite(WeaponState  w);
    void implRead (WeaponState &w);

    // composite types
    void implWrite(const Tempest::Vec3& s) { if(dump!=nullptr) dumpFloats(&s,sizeof(s)); else writeBytes(&s,sizeof(s)); }
    void implRead (Tempest::Vec3& s)       { readBytes (&s,sizeof(s)); }

    void implWrite(const Tempest::Matrix4x4& i) { if(dump!=nullptr) dumpFloats(&i,sizeof(i)); else writeBytes(&i,sizeof(i)); }
    void implRead (Tempest::Matrix4x4& i)       { readBytes (&i,sizeof(i)); }

    void implWrite(const zenkit::AnimationSample& i);
    void implRead (zenkit::AnimationSample& i);

    // strings
    void implWrite(const std::string&              s);
    void implRead (std::string&                    s);

    void implWrite(std::string_view                s);

    // vectors
    template<class T>
    void implWrite(const std::vector<T>& s) {
      implWriteVec(s,std::is_trivial<T>());
      }

    template<class T>
    void implRead (std::vector<T>& s){
      implReadVec(s,std::is_trivial<T>());
      }

    void implWrite(const std::vector<bool>& s) {
      uint32_t sz=uint32_t(s.size());
      write(sz);
      for(size_t i=0; i<s.size(); ++i)
        write(s[i]);
      }

    void implRead (std::vector<bool>& s) {
      uint32_t sz=0;
      read(sz);
      s.resize(sz);
      for(size_t i=0; i<s.size(); ++i) {
        bool b=false;
        read(b);
        s[i] = b;
        }
      }

    template<class T>
    void implWriteVec(const std::vector<T>& s,std::false_type) {
      uint32_t sz=uint32_t(s.size());
      write(sz);
      for(auto& i:s)
        write(i);
      }

    template<class T>
    void implWriteVec(const std::vector<T>& s,std::true_type) {
      uint32_t sz=uint32_t(s.size());
      write(sz);
      if(dump!=nullptr)
        dumpTrivial(s.data(),sz);
      else
        writeBytes(s.data(),sz*sizeof(T));
      }

    template<class T>
    void implReadVec(std::vector<T>& s,std::false_type) {
      uint32_t sz=0;
      read(sz);
      s.resize(sz);
      for(auto& i:s)
        read(i);
      }

    template<class T>
    void implReadVec(std::vector<T>& s,std::true_type) {
      uint32_t sz=0;
      read(sz);
      s.resize(sz);
      readBytes(s.data(),sz*sizeof(T));
      }

    // c-arrays
    template<class T, size_t sz>
    void implWrite(const T (&s)[sz]) {
      implWriteArr(s,std::is_trivial<T>());
      }

    template<class T, size_t sz>
    void implWriteArr(const T (&s)[sz],std::false_type) {
      for(size_t i=0; i<sz; ++i)
        write(s[i]);
      }

    template<class T, size_t sz>
    void implWriteArr(const T (&s)[sz],std::true_type) {
      if(dump!=nullptr)
        dumpTrivial(s,sz);
      else
        writeBytes(s,sz*sizeof(T));
      }

    // Arithmetic and enum elements one by one, anything else as its bytes.
    template<class T>
    void dumpTrivial(const T* s, size_t n) {
      for(size_t i=0; i<n; ++i) {
        if constexpr(std::is_same_v<T,bool>)
          dumpRaw(s[i] ? "true" : "false");
        else if constexpr(std::is_same_v<T,float>)
          dumpFloat(s[i]);
        else if constexpr(std::is_enum_v<T>)
          dumpTrivial(reinterpret_cast<const std::underlying_type_t<T>*>(s+i),1);
        else if constexpr(std::is_integral_v<T> && std::is_signed_v<T>)
          dumpNum(int64_t(s[i]));
        else if constexpr(std::is_integral_v<T>)
          dumpNum(uint64_t(s[i]));
        else {
          dumpHex(s,n*sizeof(T));
          return;
          }
        }
      }

    void dumpRaw   (std::string_view token);
    void dumpNum   (int64_t  v);
    void dumpNum   (uint64_t v);
    void dumpFloat (float f);
    void dumpFloats(const void* v, size_t sz);
    void dumpHex   (const void* v, size_t sz);

    template<class T, size_t sz>
    void implRead(T (&s)[sz]) {
      implReadArr(s,std::is_trivial<T>());
      }

    template<class T, size_t sz>
    void implReadArr(T (&s)[sz],std::false_type) {
      for(size_t i=0; i<sz; ++i)
        read(s[i]);
      }

    template<class T, size_t sz>
    void implReadArr(T (&s)[sz],std::true_type) {
      readBytes(s,sz*sizeof(T));
      }

    void implWrite(const SaveGameHeader& p);
    void implRead (SaveGameHeader&       p);

    void implWrite(const Tempest::Pixmap& p);
    void implWrite(const Tempest::Pixmap& p, const char* ext);
    template<class Ext>
    void implWrite(std::tuple<const Tempest::Pixmap&, Ext> p) { implWrite(std::get<0>(p), std::get<1>(p)); }
    void implRead (Tempest::Pixmap&       p);

    void implWrite(const zenkit::INpc& h);

    void implWrite(const FpLock& fp);
    void implRead (FpLock& fp);

    // pointers
    void implWrite(const WayPoint*  wptr);
    void implRead (const WayPoint*& wptr);

    void implWrite(const ScriptFn& fn);
    void implRead (ScriptFn&       fn);

    void implWrite(const Npc*  npc);
    void implRead (const Npc*& npc);

    void implWrite(Npc*        npc);
    void implRead (Npc*&       npc);

    void implWrite(Interactive*  mobsi);
    void implRead (Interactive*& mobsi);

    static size_t writeFunc(void *pOpaque, uint64_t file_ofs, const void *pBuf, size_t n);
    static size_t readFunc (void *pOpaque, uint64_t file_ofs, void *pBuf, size_t n);

    void   closeEntry();
    bool   implSetEntry(std::string_view e);
    uint32_t implDirectorySize(std::string_view e);

    uint16_t                 curVer = Version::Current;
    uint16_t                 wldVer = Version::Current;

    std::string              tmpStr;
    std::unordered_set<std::string> outFileList;
    World*                   ctx       = nullptr;

    mz_zip_archive           impl      = {};
    std::string              entryName;
    std::vector<uint8_t>     entryBuf;
    uint64_t                 curOffset = 0;
    uint64_t                 readOffset = 0;
    Tempest::ODevice*        fout      = nullptr;
    Tempest::IDevice*        fin       = nullptr;
    std::string*             dump      = nullptr;
    bool                     dumpEntry = false;  // an entry's value list is open
    bool                     dumpFirst = true;   // no value yet in the open list (or no entry yet)
  };

