#include "zipx.h"

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <miniz.h>   // the ROM's (esp_rom/include)

namespace burner {
namespace zipx {
namespace {

constexpr uint32_t kEndSig = 0x06054b50, kCentralSig = 0x02014b50, kLocalSig = 0x04034b50;
constexpr size_t kInBuf = 16384;

uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Every folder on the way to path.
void makeParents(const std::string &path) {
  for (size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
    mkdir(path.substr(0, p).c_str(), 0777);
}

// A zip entry name made safe: forward slashes, no leading slash, and no ".."
// or "." parts, so nothing lands outside the destination. "" if nothing is left.
std::string safeName(std::string n) {
  for (char &c : n)
    if (c == '\\') c = '/';
  std::string out, part;
  n += '/';
  for (char c : n) {
    if (c != '/') {
      part += c;
      continue;
    }
    if (!part.empty() && part != "." && part != "..") out += (out.empty() ? "" : "/") + part;
    part.clear();
  }
  return out;
}

struct Buffers {
  uint8_t *in = nullptr, *dict = nullptr;
  tinfl_decompressor *d = nullptr;
  Buffers() {
    in = (uint8_t *)heap_caps_malloc(kInBuf, MALLOC_CAP_SPIRAM);
    dict = (uint8_t *)heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
    d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  }
  ~Buffers() {
    free(in);
    free(dict);
    free(d);
  }
  bool ok() const { return in && dict && d; }
};

// Copies (stored) or inflates (deflated) one entry's data from zip to out.
bool unpack(FILE *zip, FILE *out, int method, uint32_t csize, uint32_t usize, uint32_t want_crc, Buffers &b,
            std::string *why) {
  uint32_t crc = 0, written = 0, left = csize;
  if (method == 0) {
    while (left) {
      const size_t n = fread(b.in, 1, left < kInBuf ? left : kInBuf, zip);
      if (!n || fwrite(b.in, 1, n, out) != n) {
        *why = "read/write error";
        return false;
      }
      crc = esp_rom_crc32_le(crc, b.in, n);
      written += n;
      left -= n;
    }
  } else if (method == 8) {
    tinfl_init(b.d);
    size_t in_avail = 0, in_pos = 0, out_pos = 0;
    for (;;) {
      if (!in_avail && left) {
        in_avail = fread(b.in, 1, left < kInBuf ? left : kInBuf, zip);
        if (!in_avail) {
          *why = "the zip is cut short";
          return false;
        }
        in_pos = 0;
        left -= in_avail;
      }
      size_t in_n = in_avail, out_n = TINFL_LZ_DICT_SIZE - out_pos;
      const tinfl_status st = tinfl_decompress(b.d, b.in + in_pos, &in_n, b.dict, b.dict + out_pos, &out_n,
                                               left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      in_avail -= in_n;
      in_pos += in_n;
      if (out_n) {
        if (fwrite(b.dict + out_pos, 1, out_n, out) != out_n) {
          *why = "card write error";
          return false;
        }
        crc = esp_rom_crc32_le(crc, b.dict + out_pos, out_n);
        written += out_n;
        out_pos = (out_pos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
      }
      if (st == TINFL_STATUS_DONE) break;
      if (st < 0) {
        *why = "corrupt data";
        return false;
      }
      if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !in_avail && !left) {
        *why = "the zip is cut short";
        return false;
      }
    }
  } else {
    *why = "compression method " + std::to_string(method) + " (only stored and deflate)";
    return false;
  }
  if (written != usize || crc != want_crc) {
    *why = "CRC or size mismatch";
    return false;
  }
  return true;
}

}  // namespace

std::string stem(const std::string &file_name) {
  const size_t dot = file_name.rfind('.');
  return dot == std::string::npos ? file_name : file_name.substr(0, dot);
}

bool isZip(const std::string &name) {
  return name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".zip") == 0;
}

Result extract(const std::string &zip_path, const std::string &dest_dir,
               std::function<void(const std::string &, int, int)> progress) {
  Result r;
  FILE *zip = fopen(zip_path.c_str(), "rb");
  if (!zip) {
    r.error = "cannot open the zip";
    return r;
  }
  // The end-of-central-directory record sits in the last 64 KB + 22 bytes.
  fseek(zip, 0, SEEK_END);
  const long size = ftell(zip);
  const long tail = size < 65557 ? size : 65557;
  std::vector<uint8_t> t(tail);
  fseek(zip, size - tail, SEEK_SET);
  if (fread(t.data(), 1, tail, zip) != (size_t)tail) {
    fclose(zip);
    r.error = "cannot read the zip";
    return r;
  }
  long eocd = -1;
  for (long i = tail - 22; i >= 0; i--)
    if (u32(&t[i]) == kEndSig) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    fclose(zip);
    r.error = "not a zip file";
    return r;
  }
  const int count = u16(&t[eocd + 10]);
  const uint32_t cd_off = u32(&t[eocd + 16]);
  if (count == 0xFFFF || cd_off == 0xFFFFFFFF) {
    fclose(zip);
    r.error = "zip64 is not supported";
    return r;
  }

  Buffers b;
  if (!b.ok()) {
    fclose(zip);
    r.error = "out of memory";
    return r;
  }
  mkdir(dest_dir.c_str(), 0777);

  // First pass over the names: when everything sits in one top-level folder
  // (what Finder's Compress makes of a folder), drop that level, so
  // "roms.zip" holding "roms/a.bin" unpacks to roms/a.bin, not roms/roms/a.bin.
  std::string strip;
  {
    long p = cd_off;
    bool single = true;
    for (int i = 0; i < count && single; i++) {
      uint8_t h[46];
      fseek(zip, p, SEEK_SET);
      if (fread(h, 1, 46, zip) != 46 || u32(h) != kCentralSig) break;
      const int nlen = u16(h + 28);
      std::string name(nlen, '\0');
      fread(&name[0], 1, nlen, zip);
      p += 46 + nlen + u16(h + 30) + u16(h + 32);
      const std::string safe = safeName(name);
      if (safe.empty() || safe.compare(0, 8, "__MACOSX") == 0) continue;
      const size_t slash = safe.find('/');
      const bool is_dir_entry = name.back() == '/' || name.back() == '\\';
      const std::string top = slash == std::string::npos ? (is_dir_entry ? safe : "") : safe.substr(0, slash);
      if (top.empty()) single = false;                 // a file at the top level
      else if (strip.empty()) strip = top;
      else if (strip != top) single = false;
    }
    if (!single) strip.clear();
  }

  long cd = cd_off;
  for (int i = 0; i < count; i++) {
    uint8_t h[46];
    fseek(zip, cd, SEEK_SET);
    if (fread(h, 1, 46, zip) != 46 || u32(h) != kCentralSig) {
      r.error = "damaged central directory";
      break;
    }
    const int flags = u16(h + 8), method = u16(h + 10);
    const uint32_t crc = u32(h + 16), csize = u32(h + 20), usize = u32(h + 24);
    const int nlen = u16(h + 28), xlen = u16(h + 30), clen = u16(h + 32);
    const uint32_t local = u32(h + 42);
    std::string name(nlen, '\0');
    fread(&name[0], 1, nlen, zip);
    cd += 46 + nlen + xlen + clen;

    const bool is_dir = !name.empty() && (name.back() == '/' || name.back() == '\\');
    std::string safe = safeName(name);
    if (safe.empty() || safe.compare(0, 9, "__MACOSX/") == 0 || safe == "__MACOSX") continue;
    if (!strip.empty()) {
      if (safe == strip) continue;                     // the folder itself
      safe = safe.substr(strip.size() + 1);
    }
    if (progress) progress(safe, i, count);
    const std::string out_path = dest_dir + "/" + safe;
    if (is_dir) {
      makeParents(out_path + "/");
      continue;
    }
    if (flags & 1) {
      r.error = safe + ": encrypted entries are not supported";
      break;
    }
    // The data follows the local header, whose name/extra lengths can differ
    // from the central directory's.
    uint8_t lh[30];
    fseek(zip, local, SEEK_SET);
    if (fread(lh, 1, 30, zip) != 30 || u32(lh) != kLocalSig) {
      r.error = safe + ": damaged local header";
      break;
    }
    fseek(zip, local + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET);
    makeParents(out_path);
    FILE *out = fopen(out_path.c_str(), "wb");
    if (!out) {
      r.error = safe + ": cannot create the file";
      break;
    }
    std::string why;
    const bool ok = unpack(zip, out, method, csize, usize, crc, b, &why);
    fclose(out);
    if (!ok) {
      remove(out_path.c_str());
      r.error = safe + ": " + why;
      break;
    }
    r.files++;
    r.bytes += usize;
  }
  fclose(zip);
  r.ok = r.error.empty();
  if (!r.ok && r.files == 0) rmdir(dest_dir.c_str());   // nothing came out: leave no empty folder
  return r;
}

}  // namespace zipx
}  // namespace burner
