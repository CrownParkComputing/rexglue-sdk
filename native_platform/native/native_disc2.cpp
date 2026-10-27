// Read-only FM4 marketplace STFS extraction. Block layout follows the BSD
// licensed Xenia/ReXGlue STFS implementation (Ben Vanik and contributors).
// Copyright 2020 Ben Vanik. All rights reserved. Adapted by Tom Clay, 2026.
// See licenses/STFS-BSD.txt and the original stfs_container_device.cpp.
#include "native_disc2.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace forza4 {
namespace fs = std::filesystem;
namespace {
uint32_t BE(const uint8_t* p, int n=4) { uint32_t v=0; while(n--) v=(v<<8)|*p++; return v; }
uint32_t LE24(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16); }
bool Component(const std::string& s) {
  return !s.empty() && s!="." && s!=".." && s.find_first_of("/\\:")==std::string::npos &&
      std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32;});
}
struct Entry { fs::path path; bool directory; uint32_t block, size; };
class Package {
 public:
  std::ifstream file;
  uint64_t size, base;
  uint32_t blocks, table, tables;
  std::array<uint8_t,0xb000> header{};
  std::map<uint64_t,std::array<uint8_t,4096>> hash_cache;
  explicit Package(const fs::path& p):file(p,std::ios::binary),size(fs::file_size(p)) {
    Read(0,header.data(),header.size());
    if ((std::memcmp(header.data(),"PIRS",4) && std::memcmp(header.data(),"LIVE",4)) ||
        BE(&header[0x344])!=2 || BE(&header[0x360])!=0x4d530910 ||
        BE(&header[0x3a9])!=0 || header[0x379]!=0x24 || !(header[0x37b]&1))
      throw std::runtime_error("Expected a read-only Forza 4 Disc 2 marketplace package.");
    base=(uint64_t(BE(&header[0x340]))+4095)&~uint64_t(4095);
    if (base<header.size() || base>=size) throw std::runtime_error("Invalid STFS header length.");
    blocks=BE(&header[0x395]);
    tables=header[0x37c]|(header[0x37d]<<8); table=LE24(&header[0x37e]);
    if (!blocks || blocks>size/4096 || !tables || tables>blocks)
      throw std::runtime_error("Invalid STFS volume dimensions.");
  }
  void Read(uint64_t offset, void* data, size_t length) {
    if (offset>size || length>size-offset) throw std::runtime_error("STFS block lies outside the package.");
    file.clear(); file.seekg(offset);
    if (!file.read(static_cast<char*>(data),length)) throw std::runtime_error("Cannot read STFS package.");
  }
  uint64_t Offset(uint32_t index) {
    if(index>=blocks) throw std::runtime_error("Invalid STFS data block.");
    uint64_t b=index,level=170;
    for(int i=0;i<3;++i) { b+=(index+level)/level; if(index<level) break; level*=170; }
    return base+b*4096;
  }
  uint32_t Next(uint32_t index) {
    if(index>=blocks) throw std::runtime_error("Invalid STFS hash reference.");
    uint64_t h=0;
    if(index>=170) {
      h=uint64_t(index/170)*171+index/28900+1;
      if(index>=28900) ++h;
    }
    const uint64_t offset=base+h*4096;
    auto [it,inserted]=hash_cache.try_emplace(offset);
    if(inserted) Read(offset,it->second.data(),4096);
    return BE(it->second.data()+(index%170)*24+20)&0xffffff;
  }
  std::vector<Entry> Entries() {
    std::vector<Entry> entries;
    std::set<uint32_t> visited;
    std::set<std::string> names;
    uint32_t block=table;
    for(uint32_t n=0;n<tables;++n) {
      if(!visited.insert(block).second) throw std::runtime_error("Cyclic STFS directory chain.");
      std::array<uint8_t,4096> data; Read(Offset(block),data.data(),data.size());
      for(int i=0;i<64;++i) {
        const uint8_t* e=data.data()+i*64;
        if(!e[0]) break;
        const uint32_t length=e[40]&63;
        if(length>40) throw std::runtime_error("Invalid STFS file name.");
        std::string name(reinterpret_cast<const char*>(e),length);
        if(!Component(name)) throw std::runtime_error("Unsafe STFS file name.");
        const uint32_t parent=BE(e+50,2);
        fs::path path=name;
        if(parent!=0xffff) {
          if(parent>=entries.size() || !entries[parent].directory)
            throw std::runtime_error("Invalid STFS parent directory.");
          path=entries[parent].path/name;
        }
        std::string key=path.generic_string();
        std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
        if(!names.insert(key).second) throw std::runtime_error("Duplicate STFS file name.");
        entries.push_back({path,bool(e[40]&128),LE24(e+47),BE(e+52)});
      }
      if(n+1<tables) block=Next(block);
    }
    if(entries.empty()) throw std::runtime_error("Empty STFS file table.");
    return entries;
  }
};
void Extract(const fs::path& source,const fs::path& destination,
             const std::function<void(const std::string&)>& progress) {
  Package p(source);
  const auto entries=p.Entries();
  uint64_t total=0; for(const auto& e:entries) if(!e.directory) total+=e.size;
  if(total>p.size) throw std::runtime_error("STFS files exceed the package size.");
  fs::create_directories(destination.parent_path());
  if(fs::space(destination.parent_path()).available<total+16*1024*1024)
    throw std::runtime_error("Not enough free space to install Disc 2.");
  const fs::path staging=destination.parent_path()/("."+destination.filename().string()+".installing");
  // A partial extraction is never published to the content enumerator.
  if(fs::exists(staging)) fs::remove_all(staging);
  struct Cleanup { fs::path path; ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);} } cleanup{staging};
  fs::create_directories(staging/"files");
  uint64_t done=0,report_at=0;
  std::array<uint8_t,4096> data;
  for(const auto& e:entries) {
    const auto output=staging/"files"/e.path;
    if(e.directory) {fs::create_directories(output);continue;}
    fs::create_directories(output.parent_path());
    std::ofstream f(output,std::ios::binary|std::ios::trunc);
    uint32_t left=e.size,block=e.block;
    std::set<uint32_t> visited;
    while(left) {
      if(!visited.insert(block).second) throw std::runtime_error("Cyclic STFS file chain.");
      const uint32_t count=std::min(4096u,left);
      p.Read(p.Offset(block),data.data(),count);
      if(!f.write(reinterpret_cast<const char*>(data.data()),count)) throw std::runtime_error("Cannot write installed content.");
      left-=count; done+=count;
      if(left) block=p.Next(block);
      if(progress && done>=report_at) {
        progress(std::format("Installing {}: {}%",source.filename().string(),total?done*100/total:100));
        report_at=done+16*1024*1024;
      }
    }
    f.close(); if(!f) throw std::runtime_error("Cannot finish installed content file.");
  }
  std::array<uint8_t,308> metadata{};
  metadata[3]=1;metadata[7]=2;
  std::memcpy(metadata.data()+8,p.header.data()+0x411,256);
  const auto name=source.filename().string();
  std::memcpy(metadata.data()+264,name.data(),name.size());
  std::ofstream m(staging/"metadata.bin",std::ios::binary);
  m.write(reinterpret_cast<const char*>(metadata.data()),metadata.size());m.close();
  std::ofstream marker(staging/"native-disc2.complete");
  marker<<"FM4-STFS-v1 "<<p.size<<'\n';marker.close();
  if(!m || !marker) throw std::runtime_error("Cannot save Disc 2 metadata.");
  if(fs::exists(destination)) throw std::runtime_error("An incomplete Disc 2 destination already exists; choose another save directory.");
  fs::rename(staging,destination);
}
}
Disc2Result InstallNativeDisc2(const fs::path& source,const fs::path& game_root,
    const fs::path& save_root,const std::function<void(const std::string&)>& progress) {
  Disc2Result result;
  try {
    std::vector<fs::path> roots;
    if(!source.empty()) roots.push_back(source);
    else {
      roots.push_back(game_root/"Disc2/content/0000000000000000/4d530910/00000002");
      roots.push_back(game_root/"content/0000000000000000/4d530910/00000002");
    }
    std::map<std::string,fs::path> packages;
    for(const auto& root:roots) {
      std::error_code ec;
      fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;
      while(!ec && it!=end) {
        if(it.depth()>8) it.disable_recursion_pending();
        std::string name=it->path().filename().string();
        if(it->is_regular_file(ec)) for(int i=1;i<=4;++i)
          if(name==std::format("4d530910{:08x}",i)) packages.try_emplace(name,it->path());
        it.increment(ec);
      }
    }
    if(packages.empty()) throw std::runtime_error("No Disc 2 packages found. Choose the extracted Disc 2 folder.");
    for(const auto& [name,path]:packages) {
      Package validate(path);
      const auto dest=save_root/"content/00000002"/name;
      std::ifstream marker(dest/"native-disc2.complete");
      std::string tag; uint64_t size=0;marker>>tag>>size;
      if(tag=="FM4-STFS-v1" && size==validate.size && fs::is_directory(dest/"files") &&
          fs::is_regular_file(dest/"metadata.bin") && fs::file_size(dest/"metadata.bin")==308) ++result.existing;
      else { Extract(path,dest,progress);++result.installed; }
    }
    result.ok=true;
    result.message=std::format("Disc 2: {}/4 packages ready ({} newly installed).",result.installed+result.existing,result.installed);
  } catch(const std::exception& e) {result.message=e.what();}
  if(progress) progress(result.message);
  return result;
}
}
