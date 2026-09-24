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
