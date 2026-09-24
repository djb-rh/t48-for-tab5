// Runs minipro (its own main(), see core/minipro_main.c) in a task of its own
// and captures what it prints, so the UI can show a log and a progress bar
// while the T48 works.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace burner {
namespace runner {

void begin();

// Starts minipro with these arguments (argv[0] is supplied). False if a run
// is already going. `title` names the job on screen ("Write", "Read"...).
bool start(const std::vector<std::string> &args, const char *title);
bool busy();

struct Status {
  bool busy = false;
  std::string title;       // of the current or last job
  std::string phase;       // e.g. "Writing Code..." while one is in progress
  int percent = -1;        // of the current phase, -1 when there is none
  int rc = 0;              // minipro's exit code for the last finished job
  bool finished = false;   // a job has finished since boot
  uint32_t elapsed_ms = 0;
  uint32_t seq = 0;        // bumps whenever anything here changes
};
Status status();

// The last lines minipro printed, oldest first.
std::vector<std::string> lines(int max);
void clearLog();
// Adds a line of the app's own to the same log.
void note(const char *fmt, ...);

// While true, battery charging should be paused: the T48 gets 4.4 V instead
// of 5 V while the battery charges. main.cpp owns the charger (I2C).
bool wantsChargePaused();
void setChargePaused(bool paused);   // main.cpp reports what it did

}  // namespace runner
}  // namespace burner
