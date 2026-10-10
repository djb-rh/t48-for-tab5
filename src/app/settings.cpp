#include "settings.h"

#include <Preferences.h>

namespace burner {
namespace settings {
namespace {

Settings g_s;
bool g_loaded = false;

void load() {
  Preferences p;
  p.begin("burner", true);
  g_s.part_name = p.getString("part", "").c_str();
  g_s.part_maker = p.getString("maker", "").c_str();
  g_s.image = p.getString("image", "").c_str();
  const uint32_t o = p.getUInt("opts", 0);
  g_s.opt.ignore_size = o & 1;
  g_s.opt.skip_erase = o & 2;
  g_s.opt.skip_verify = o & 4;
  g_s.opt.ignore_id = o & 8;
  g_s.wifi_ssid = p.getString("ssid", "").c_str();
  g_s.wifi_pass = p.getString("pass", "").c_str();
  g_s.onerom.type = p.getString("or_type", "27C512").c_str();
  const uint32_t oc = p.getUInt("or_cs", 0);
  for (int i = 0; i < 3; i++) g_s.onerom.cs[i] = (oc >> (i * 2)) & 3;
  g_s.onerom.fit = p.getUInt("or_fit", 0);
  p.end();
  g_loaded = true;
}

}  // namespace

Settings &get() {
  if (!g_loaded) load();
  return g_s;
}

void save() {
  Preferences p;
  p.begin("burner", false);
  p.putString("part", g_s.part_name.c_str());
  p.putString("maker", g_s.part_maker.c_str());
  p.putString("image", g_s.image.c_str());
  p.putUInt("opts", (g_s.opt.ignore_size ? 1 : 0) | (g_s.opt.skip_erase ? 2 : 0) |
                        (g_s.opt.skip_verify ? 4 : 0) | (g_s.opt.ignore_id ? 8 : 0));
  p.putString("ssid", g_s.wifi_ssid.c_str());
  p.putString("pass", g_s.wifi_pass.c_str());
  p.putString("or_type", g_s.onerom.type.c_str());
  p.putUInt("or_cs", g_s.onerom.cs[0] | g_s.onerom.cs[1] << 2 | g_s.onerom.cs[2] << 4);
  p.putUInt("or_fit", g_s.onerom.fit);
  p.end();
}

}  // namespace settings
}  // namespace burner
