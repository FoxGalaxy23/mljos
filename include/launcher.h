#ifndef LAUNCHER_H
#define LAUNCHER_H

// Launches a GUI windowed app by name (e.g. "calc", "time", or "terminal").
// Returns 1 on success.
int launcher_launch_gui(const char *name);

// Launches a GUI windowed app and provides a path for it to open
int launcher_launch_gui_args(const char *name, const char *open_path);

#endif

