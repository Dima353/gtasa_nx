#ifndef __HOOKS_H__
#define __HOOKS_H__

void patch_game(void);

// Terminate the process, skipping the mobile engine's crashy teardown (see main.c).
// Commits the SD first so any just-written save persists. Never returns.
void hard_exit(void);

void deinit_openal(void);

// Queue a cheat code (from the on-screen keyboard in main.c); fed to the game's
// CCheat::AddToCheatString by the DoCheats hook. Defined in hooks/game.c.
void cheats_enqueue(const char *s);

#endif
