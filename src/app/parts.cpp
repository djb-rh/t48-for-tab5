#include "parts.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace burner {
namespace parts {
namespace {

constexpr const char *kMarker = "<!--TAB5-PART-->";

char *g_buf = nullptr;        // parts.idx, tabs and newlines turned into NULs
std::vector<Row> g_rows;
std::string g_info_skel, g_logic_skel;

bool readFile(const char *path, std::string *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize(n);
  const bool ok = fread(&(*out)[0], 1, n, f) == (size_t)n;
  fclose(f);
  return ok;
}

bool writeFile(const char *path, const std::string &data) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  return fclose(f) == 0 && ok;
}

// Case-insensitive "does hay contain needle", with needle already upper case.
bool containsUpper(const char *hay, const char *needle, size_t nlen) {
  for (; *hay; hay++) {
    size_t k = 0;
    while (k < nlen && hay[k] && toupper((unsigned char)hay[k]) == needle[k]) k++;
    if (k == nlen) return true;
  }
  return false;
}

bool startsUpper(const char *hay, const char *needle, size_t nlen) {
  for (size_t k = 0; k < nlen; k++)
    if (!hay[k] || toupper((unsigned char)hay[k]) != needle[k]) return false;
  return true;
}

}  // namespace

bool load(std::string *why) {
  if (g_buf) return true;
  char path[96];
  snprintf(path, sizeof(path), "%s/parts.idx", kDbDir);
  FILE *f = fopen(path, "rb");
  if (!f) {
    *why = "No part library on the card (burner/db/parts.idx)";
    return false;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!g_buf || fread(g_buf, 1, n, f) != (size_t)n) {
    fclose(f);
    free(g_buf);
    g_buf = nullptr;
    *why = "Could not read parts.idx";
    return false;
  }
  fclose(f);
  g_buf[n] = 0;

  g_rows.clear();
  g_rows.reserve(32000);
  char *p = g_buf;
  char *end = g_buf + n;
  while (p < end) {
    char *field[6];
    int k = 0;
    field[k++] = p;
    while (p < end && *p != '\n') {
      if (*p == '\t' && k < 6) {
        *p = 0;
        field[k++] = p + 1;
      }
      p++;
    }
    if (p < end) *p++ = 0;
    if (k < 6) continue;
    g_rows.push_back(Row{field[0], field[1], field[2], (uint32_t)strtoul(field[3], nullptr, 10),
                         (uint32_t)strtoul(field[4], nullptr, 10), (uint32_t)strtoul(field[5], nullptr, 10)});
  }

  snprintf(path, sizeof(path), "%s/infoic.xml", kDbDir);
  if (!readFile(path, &g_info_skel)) {
    *why = "Could not read burner/db/infoic.xml";
    return false;
  }
  snprintf(path, sizeof(path), "%s/logicic.xml", kDbDir);
  if (!readFile(path, &g_logic_skel)) {
    *why = "Could not read burner/db/logicic.xml";
    return false;
  }
  mkdir("/sdcard/burner/sel", 0777);
  return true;
}

bool loaded() { return g_buf != nullptr; }
int count() { return (int)g_rows.size(); }
const Row &row(int i) { return g_rows[i]; }

void search(const std::string &query, std::vector<int> *out) {
  out->clear();
  // Split into upper-case words.
  std::vector<std::string> words;
  std::string w;
  for (char c : query) {
    if (c == ' ') {
      if (!w.empty()) words.push_back(w);
      w.clear();
    } else {
      w += (char)toupper((unsigned char)c);
    }
  }
  if (!w.empty()) words.push_back(w);
  if (words.empty()) {
    out->resize(g_rows.size());
    for (size_t i = 0; i < g_rows.size(); i++) (*out)[i] = (int)i;
    return;
  }
  std::vector<int> later;
  for (size_t i = 0; i < g_rows.size(); i++) {
    const char *name = g_rows[i].name;
    bool all = true;
    for (auto &x : words) {
      if (!containsUpper(name, x.c_str(), x.size())) {
        all = false;
        break;
      }
    }
    if (!all) continue;
    // "27C512" should list 27C512@DIP28 before AM27C512@DIP28.
    if (startsUpper(name, words[0].c_str(), words[0].size())) out->push_back((int)i);
    else later.push_back((int)i);
  }
  out->insert(out->end(), later.begin(), later.end());
}

bool select(int i, std::string *why) {
  const Row &r = g_rows[i];
  char path[96];
  snprintf(path, sizeof(path), "%s/entries.xml", kDbDir);
  FILE *f = fopen(path, "rb");
  if (!f) {
    *why = "Could not open entries.xml";
    return false;
  }
  std::string entry(r.length, '\0');
  const bool ok = fseek(f, r.offset, SEEK_SET) == 0 && fread(&entry[0], 1, r.length, f) == r.length;
  fclose(f);
  if (!ok || entry.compare(0, 3, "<ic") != 0) {
    *why = "The part entry is unreadable; rebuild the library";
    return false;
  }
  // The entry goes in the database it came from; the other gets none.
  const bool logic = !strcmp(r.kind, "Logic");
  std::string info = g_info_skel, lgc = g_logic_skel;
  const size_t mi = info.find(kMarker), ml = lgc.find(kMarker);
  if (mi == std::string::npos || ml == std::string::npos) {
    *why = "The library skeletons have no marker; rebuild the library";
    return false;
  }
  info.replace(mi, strlen(kMarker), logic ? "" : entry);
  lgc.replace(ml, strlen(kMarker), logic ? entry : "");
  if (!writeFile(kSelInfoic, info) || !writeFile(kSelLogicic, lgc)) {
    *why = "Could not write the part files to the card";
    return false;
  }
  return true;
}

int find(const char *name, const char *maker) {
  // The index is sorted by upper-cased name; a linear scan is a few ms.
  for (size_t i = 0; i < g_rows.size(); i++)
    if (!strcmp(g_rows[i].name, name) && !strcmp(g_rows[i].maker, maker)) return (int)i;
  return -1;
}

namespace {

uint32_t attrNum(const char *line, const char *name) {
  char key[32];
  snprintf(key, sizeof(key), " %s=\"", name);
  const char *p = strstr(line, key);
  return p ? (uint32_t)strtoul(p + strlen(key), nullptr, 0) : 0xFFFFFFFF;
}

// Every entry's chip ID and protocol, by its offset in entries.xml: built on
// the first lookup (reading the file in 64 KB blocks; line by line through
// stdio took long enough on the card to look like a hang) and kept.
struct IdRow {
  uint32_t chip_id, protocol, offset;
};
std::vector<IdRow> g_ids;
bool g_ids_built = false;

void buildIds() {
  if (g_ids_built) return;
  char path[96];
  snprintf(path, sizeof(path), "%s/entries.xml", kDbDir);
  FILE *f = fopen(path, "rb");
  if (!f) return;
  constexpr size_t kBlock = 65536;
  char *buf = (char *)heap_caps_malloc(kBlock + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    fclose(f);
    return;
  }
  g_ids.clear();
  g_ids.reserve(12000);
  size_t have = 0;       // bytes in buf not yet consumed
  uint32_t base = 0;     // file offset of buf[0]
  bool eof = false;
  while (!eof || have) {
    if (!eof) {
      const size_t n = fread(buf + have, 1, kBlock - have, f);
      if (n == 0) eof = true;
      have += n;
    }
    buf[have] = 0;
    size_t start = 0;
    for (;;) {
      char *nl = (char *)memchr(buf + start, '\n', have - start);
      if (!nl) {
        // A line longer than the block, or the file's last line: take what
        // there is when nothing more is coming.
        if ((eof || start == 0) && start < have) nl = buf + have;
        else break;
      }
      *nl = 0;
      const char *line = buf + start;
      if (line[0] == '<' && line[1] == 'i' && line[2] == 'c') {
        const uint32_t cid = attrNum(line, "chip_id");
        // Every entry, ID or not: the chosen part's protocol is looked up here too.
        g_ids.push_back({cid, attrNum(line, "protocol_id"), base + (uint32_t)start});
      }
      start = (size_t)(nl - buf) + 1;
      if (start >= have) break;
    }
    if (start > have) start = have;
    memmove(buf, buf + start, have - start);
    base += start;
    have -= start;
    if (eof && have == 0) break;
  }
  free(buf);
  fclose(f);
  g_ids_built = true;
}

}  // namespace

std::vector<int> byChipId(uint32_t id, int like) {
  std::vector<int> out;
  buildIds();
  // The protocol of the chosen part (the same algorithm family).
  uint32_t protocol = 0xFFFFFFFF;
  for (const auto &r : g_ids)
    if (like >= 0 && r.offset == g_rows[like].offset) protocol = r.protocol;
  std::vector<uint32_t> offsets;
  for (const auto &r : g_ids)
    if (r.chip_id != 0 && r.chip_id != 0xFFFFFFFF && (r.chip_id == id || (r.chip_id >> 5) == id) &&
        (protocol == 0xFFFFFFFF || r.protocol == protocol))
      offsets.push_back(r.offset);
  std::sort(offsets.begin(), offsets.end());
  for (size_t i = 0; i < g_rows.size(); i++)
    if (std::binary_search(offsets.begin(), offsets.end(), g_rows[i].offset)) out.push_back((int)i);
  return out;
}

std::string sizeText(const Row &r) {
  char b[32];
  if (!strcmp(r.kind, "Logic")) snprintf(b, sizeof(b), "%u pins", (unsigned)r.size);
  else if (r.size >= (1u << 20) && r.size % (1u << 20) == 0) snprintf(b, sizeof(b), "%u MB", (unsigned)(r.size >> 20));
  else if (r.size >= 1024 && r.size % 1024 == 0) snprintf(b, sizeof(b), "%u KB", (unsigned)(r.size >> 10));
  else snprintf(b, sizeof(b), "%u B", (unsigned)r.size);
  return b;
}

}  // namespace parts
}  // namespace burner
