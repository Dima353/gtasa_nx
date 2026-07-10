/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

// the game's `printf` import points here; a no-op with DEBUG_LOG off. The log
// file is kept open for the run (reopening per line on FAT is slow) and flushed
// each line to survive an abrupt exit.
int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  va_list list;
  static FILE *f = NULL;
  if (!f)
    f = fopen(LOG_NAME, "w"); // fresh log each boot
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fflush(f);
  }
  va_start(list, text);
  vprintf(text, list); // also to nxlink stdout, if a host is connected
  va_end(list);
#endif
  return 0;
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

// pin the calling thread to a single core. Only pins to cores actually granted
// to this process (cores 0..2 for an application; core 3 is the system core),
// so an out-of-range request just leaves the thread on its default core.
void set_thread_core(int core) {
  static u64 mask = 0;
  if (mask == 0)
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  if (core < 0 || !(mask & (1ull << core)))
    return;
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ull << core);
  if (R_FAILED(rc))
    debugPrintf("affinity: pin to core %d failed: %08x\n", core, rc);
}

// --- thread registry (see util.h) -----------------------------------------
#define MAX_TRACKED_THREADS 64
static Handle g_thread_handles[MAX_TRACKED_THREADS];
static int g_thread_count; // grow-only; index reserved with an atomic add

void thread_registry_add(void) {
  int i = __atomic_fetch_add(&g_thread_count, 1, __ATOMIC_RELAXED);
  if (i < MAX_TRACKED_THREADS)
    g_thread_handles[i] = threadGetCurHandle();
}

void thread_registry_pause_others(void) {
  Handle self = threadGetCurHandle();
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  int paused = 0;
  for (int i = 0; i < n; i++) {
    Handle h = g_thread_handles[i];
    if (h && h != self && R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)))
      paused++;
  }
  debugPrintf("EXIT: paused %d/%d engine threads\n", paused, n);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
