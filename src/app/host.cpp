#include "host.h"

namespace burner {
namespace host {

namespace {
Config g_config;
}

void set(const Config &c) { g_config = c; }
const Config &get() { return g_config; }
bool hosted() { return g_config.home != nullptr; }

}  // namespace host
}  // namespace burner
