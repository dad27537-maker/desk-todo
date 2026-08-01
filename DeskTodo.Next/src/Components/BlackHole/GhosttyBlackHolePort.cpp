// Direct integration point for sasakojiro/ghostty-blackhole-main-old.
// The upstream renderer is compiled into DeskTodo.Next.exe and entered through
// the --blackhole-render command-line mode. It remains a separate process so
// WGC never captures DeskTodo's organizer surface into itself.
#include <shellapi.h>
#define main GhosttyBlackHoleMain
#include "../../../../reference_repos/ghostty-blackhole-main-old/src/main.cpp"
