/*
 * POSIX (Linux/macOS) entry point for 007 Agent Under Fire (bond_eng.xbe).
 *
 * Same start-up sequence as main.c, without the Win32 pieces: a SIGSEGV
 * handler replaces the VEH, plain main() replaces WinMain. Used for headless
 * bring-up runs; the Windows build keeps main.c.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <ucontext.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

#ifndef AUF_ENTRY_POINT
#define AUF_ENTRY_POINT 0x0002A954
#endif
#ifndef AUF_XBE_PATH
#define AUF_XBE_PATH "game/bond_eng.xbe"
#endif
#ifndef AUF_GAME_DIR
#define AUF_GAME_DIR "game"
#endif

extern void xbe_entry_point(void);
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern int recomp_dispatch_init(void);
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    if ((sig == SIGSEGV || sig == SIGBUS) && xbox_PosixMmioFault(ucv, fault))
        return;   /* trapped hardware register access, handled */

    fprintf(stderr, "[CRASH] signal %d, fault addr=%p (Xbox VA 0x%08X)\n",
            sig, si->si_addr, (unsigned)(fault - (uintptr_t)g_xbox_mem_offset));
    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int i, shown = 0;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++)
            if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, sp[i]);
                shown++;
            }
    }
    {
        int k;
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    _exit(128 + sig);
}

static int load_xbe(const char *path, void **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *d;
    if (!f) { fprintf(stderr, "Cannot open XBE: %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    *out = d; *size = (size_t)n;
    return 1;
}

int main(void)
{
    void *xbe = NULL;
    size_t xbe_size = 0;
    struct sigaction sa;

    setenv("RECOMP_TITLE_KEVENTS", "1", 0);
    setenv("RECOMP_VBLANK", "paced", 0);
    if (!getenv("AUF_SOUND")) setenv("AUF_NOSOUND", "1", 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    if (!load_xbe(AUF_XBE_PATH, &xbe, &xbe_size)) return 1;
    printf("XBE loaded: %zu bytes\n", xbe_size);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }
    xbox_kernel_init();
    xbox_path_init(AUF_GAME_DIR, NULL);
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;

    /* D3D (XDK 4039) fence: the device pointer lives at 0x1A32A0; the title
     * bumps "submitted" at device+0x30 and D3D_BlockOnTime (0x197CE0) waits
     * for *(device+0x34) -- the GPU's "completed" -- to reach it. Nothing
     * here is a GPU, so mirror one into the other. */
    xbox_Nv2aMirrorFence(0x001A32A0u, 0x30, 0x34);

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable\n");
    xbox_WatchdogStart();

    printf("Entry point: 0x%08X\nStarting game...\n", AUF_ENTRY_POINT);
    xbe_entry_point();
    printf("Game returned.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}
