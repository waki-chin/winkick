// WinKick - GUI (pure Win32, ListView with groups/checkboxes/customdraw)
#pragma once

#include "config.hpp"
#include "cli.hpp"

namespace gui {

// Launch the main window and run the message loop.
// 'config' is already parsed; selection filters (if any in 'opts') are applied inside.
// Returns the process exit code.
int RunGui(Config config, bool isOnline, const Options& opts);

} // namespace gui
