// rexiso - list or extract an Xbox 360 disc image (XDVDFS, the "GDFX" volume)
// with the runtime's own reader, so a launcher's importer can take a plain
// .iso as well as an archive. The reader finds the game partition itself
// (raw GDF at 0, XGD2 at 0xFD90000, XGD3 at 0x2080000, plus the two older
// layouts), so the image needs no preparation.
//
//   rexiso list    <image.iso>              one line per file:  size  path
//   rexiso extract <image.iso> <out-dir>    writes the disc tree under out-dir
//
// Exit status: 0 ok, 1 not a disc image or the extraction failed, 2 usage.

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using rex::filesystem::DiscImageDevice;
using rex::filesystem::Entry;
using rex::filesystem::File;

namespace {

struct Stats {
  size_t files = 0;
  uint64_t bytes = 0;
};

bool IsDirectory(const Entry* entry) {
  return (entry->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
}

int Usage() {
  std::fprintf(stderr,
               "usage: rexiso list <image.iso>\n"
               "       rexiso extract <image.iso> <out-dir>\n");
  return 2;
}

void List(const Entry* dir, const std::string& prefix, Stats& stats) {
  for (const auto& child : dir->children()) {
    const std::string path = prefix + child->name();
    if (IsDirectory(child.get())) {
      List(child.get(), path + "/", stats);
    } else {
      std::printf("%12zu  %s\n", child->size(), path.c_str());
      stats.files++;
      stats.bytes += child->size();
    }
  }
}

bool ExtractFile(Entry* entry, const fs::path& out) {
  File* file = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kGenericRead, &file) != 0 || !file) {
    std::fprintf(stderr, "rexiso: cannot open %s in the image\n", entry->name().c_str());
    return false;
  }
  std::ofstream os(out, std::ios::binary | std::ios::trunc);
  if (!os) {
    std::fprintf(stderr, "rexiso: cannot create %s\n", out.string().c_str());
    file->Destroy();
    return false;
  }
  std::vector<uint8_t> buffer(size_t{4} << 20);
  const size_t total = entry->size();
  size_t offset = 0;
  while (offset < total) {
    const size_t want = std::min(buffer.size(), total - offset);
    size_t got = 0;
    if (file->ReadSync(std::span<uint8_t>(buffer.data(), want), offset, &got) != 0 || got == 0) {
      std::fprintf(stderr, "rexiso: read failed at %zu of %zu in %s\n", offset, total,
                   entry->name().c_str());
      file->Destroy();
      return false;
    }
    os.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(got));
    offset += got;
  }
  file->Destroy();
  os.close();
  return os.good();
}

bool Extract(const Entry* dir, const fs::path& out, Stats& stats) {
  std::error_code ec;
  fs::create_directories(out, ec);
  if (ec) {
    std::fprintf(stderr, "rexiso: cannot create %s\n", out.string().c_str());
    return false;
  }
  for (const auto& child : dir->children()) {
    const fs::path path = out / child->name();
    if (IsDirectory(child.get())) {
      if (!Extract(child.get(), path, stats)) return false;
    } else {
      if (!ExtractFile(child.get(), path)) return false;
      stats.files++;
      stats.bytes += child->size();
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string command = argv[1];
  const fs::path image = argv[2];
  if (command != "list" && command != "extract") return Usage();
  if (command == "extract" && argc < 4) return Usage();

  std::error_code ec;
  if (!fs::is_regular_file(image, ec)) {
    std::fprintf(stderr, "rexiso: %s is not a file\n", image.string().c_str());
    return 1;
  }

  rex::InitLoggingEarly();  // the reader reports through the runtime's log categories
  DiscImageDevice device("\\Device\\Cdrom0", image);
  if (!device.Initialize()) {
    std::fprintf(stderr, "rexiso: no XDVDFS volume found in %s - not an Xbox 360 disc image\n",
                 image.filename().string().c_str());
    return 1;
  }

  Stats stats;
  if (command == "list") {
    List(device.root(), "", stats);
    std::fprintf(stderr, "%zu files, %llu bytes\n", stats.files,
                 static_cast<unsigned long long>(stats.bytes));
    return 0;
  }
  if (!Extract(device.root(), fs::path(argv[3]), stats)) return 1;
  std::printf("%zu files, %llu bytes written to %s\n", stats.files,
              static_cast<unsigned long long>(stats.bytes), argv[3]);
  return 0;
}
