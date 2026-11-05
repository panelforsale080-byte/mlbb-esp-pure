#include "esp_inject.hpp"
#include "mem.hpp"
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#include <sys/user.h>

#if !defined(__aarch64__)
void esp_inject_set_scratch_dir(const char *) {}
bool esp_inject_dlopen_so(pid_t, const char *, char *errOut, size_t errCap) {
    if (errOut && errCap) std::snprintf(errOut, errCap, "inject: arm64 only");
    return false;
}
#else

static std::string g_scratchDir;

namespace {

struct MapEntry {
    uintptr_t start, end;
    std::string path;
};

static std::vector<MapEntry> read_maps(pid_t pid) {
    std::vector<MapEntry> out;
    const std::string p = pid ? "/proc/" + std::to_string(pid) + "/maps" : "/proc/self/maps";
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        MapEntry e{};
        char perms[8] = {};
        unsigned long a = 0, b = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &a, &b, perms) < 3) continue;
        e.start = (uintptr_t)a;
        e.end = (uintptr_t)b;
        const char *slash = std::strchr(line.c_str(), '/');
        if (slash) e.path = slash;
        out.push_back(std::move(e));
    }
    return out;
}

static uintptr_t find_linker_base(const std::vector<MapEntry> &maps) {
    for (const auto &e : maps) {
        if (e.path.find("linker64") != std::string::npos && e.start) return e.start;
    }
    return 0;
}

static long peek_word(pid_t pid, uintptr_t addr) {
    errno = 0;
    return ptrace(PTRACE_PEEKTEXT, pid, (void *)addr, nullptr);
}

static void poke_word(pid_t pid, uintptr_t addr, long v) {
    ptrace(PTRACE_POKETEXT, pid, (void *)addr, (void *)v);
}

static bool write_remote(pid_t pid, uintptr_t addr, const void *buf, size_t len) {
    return mem_write(pid, addr, buf, len);
}

static uintptr_t remote_syscall(pid_t pid, uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3) {
    struct user_pt_regs regs {}, saved {};
    struct iovec iov{&regs, sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) < 0) return (uintptr_t)-1;
    saved = regs;
    const uintptr_t pc = regs.pc;
    const long orig = peek_word(pid, pc);
    poke_word(pid, pc, (orig & ~0xffffffffL) | 0xD4000001L);
    regs.regs[8] = nr;
    regs.regs[0] = a0;
    regs.regs[1] = a1;
    regs.regs[2] = a2;
    regs.regs[3] = a3;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    waitpid(pid, nullptr, 0);
    ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    const uintptr_t ret = regs.regs[0];
    poke_word(pid, pc, orig);
    regs = saved;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    return ret;
}

static uintptr_t remote_call2(pid_t pid, uintptr_t func, uintptr_t a0, uintptr_t a1) {
    struct user_pt_regs regs {}, saved {};
    struct iovec iov{&regs, sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) < 0) return (uintptr_t)-1;
    saved = regs;
    const uintptr_t pc = regs.pc;
    const long o0 = peek_word(pid, pc);
    const long o1 = peek_word(pid, pc + 8);
    const uint32_t stub[2] = {0xD63F0120, 0xD4200000};
    write_remote(pid, pc, stub, sizeof(stub));
    regs.regs[0] = a0;
    regs.regs[1] = a1;
    regs.regs[9] = func;
    regs.regs[30] = pc;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    waitpid(pid, nullptr, 0);
    ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    const uintptr_t ret = regs.regs[0];
    poke_word(pid, pc, o0);
    poke_word(pid, pc + 8, o1);
    regs = saved;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    return ret;
}

static bool copy_for_inject(pid_t pid, const char *src, std::string &dst, char *err,
                              size_t errCap) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        if (err && errCap) std::snprintf(err, errCap, "open src %s", src);
        return false;
    }
    const std::string name = "mlbb_espw2s_" + std::to_string(pid) + ".so";
    std::vector<std::string> dests;
    dests.push_back("/data/local/tmp/" + name);
    if (!g_scratchDir.empty()) dests.push_back(g_scratchDir + "/" + name);

    for (const std::string &d : dests) {
        in.clear();
        in.seekg(0);
        std::ofstream out(d, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out << in.rdbuf();
        if (!out.good()) continue;
        chmod(d.c_str(), 0755);
        dst = d;
        return true;
    }
    if (err && errCap)
        std::snprintf(err, errCap, "copy inject so failed (tmp + scratch)");
    return false;
}

static bool dlopen_remote_path(pid_t pid, const char *remotePath, char *errOut, size_t errCap) {
    if (!remotePath || !remotePath[0]) return false;

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        if (errOut && errCap)
            std::snprintf(errOut, errCap, "attach: %s", std::strerror(errno));
        return false;
    }
    int st = 0;
    waitpid(pid, &st, 0);

    auto selfMaps = read_maps(0);
    auto tgtMaps = read_maps(pid);
    const uintptr_t selfL = find_linker_base(selfMaps);
    const uintptr_t tgtL = find_linker_base(tgtMaps);
    void *sym = dlsym(RTLD_DEFAULT, "__loader_dlopen");
    if (!sym) sym = dlsym(RTLD_DEFAULT, "dlopen");
    if (!selfL || !tgtL || !sym) {
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        if (errOut && errCap) std::snprintf(errOut, errCap, "dlopen symbol missing");
        return false;
    }
    const uintptr_t dlopenAddr = tgtL + ((uintptr_t)sym - selfL);

    const size_t pathLen = std::strlen(remotePath) + 1;
    const size_t mapLen = (pathLen + 0xFFF) & ~size_t(0xFFF);
    const uintptr_t remoteBuf =
        remote_syscall(pid, 222, 0, mapLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (remoteBuf <= 1) {
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        if (errOut && errCap) std::snprintf(errOut, errCap, "remote mmap failed");
        return false;
    }
    write_remote(pid, remoteBuf, remotePath, pathLen);
    const uintptr_t handle = remote_call2(pid, dlopenAddr, remoteBuf, RTLD_NOW);
    remote_syscall(pid, 215, remoteBuf, mapLen, 0, 0);

    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);

    if (!handle || handle == (uintptr_t)-1) {
        if (errOut && errCap) std::snprintf(errOut, errCap, "dlopen in game failed");
        return false;
    }
    if (errOut && errCap) std::snprintf(errOut, errCap, "ok handle=0x%llx path=%s",
                                        (unsigned long long)handle, remotePath);
    return true;
}

} // namespace

void esp_inject_set_scratch_dir(const char *appWritableDir) {
    g_scratchDir = appWritableDir ? appWritableDir : "";
}

bool esp_inject_dlopen_so(pid_t pid, const char *soPath, char *errOut, size_t errCap) {
    if (pid <= 0 || !soPath || !soPath[0]) {
        if (errOut && errCap) std::snprintf(errOut, errCap, "bad args");
        return false;
    }
    /* GSpace / shared UID: game may dlopen overlay files path directly. */
    if (dlopen_remote_path(pid, soPath, errOut, errCap)) return true;

    std::string copied;
    char copyErr[160] = {};
    if (!copy_for_inject(pid, soPath, copied, copyErr, sizeof(copyErr))) {
        if (errOut && errCap) {
            std::snprintf(errOut, errCap, "direct dlopen failed; %s", copyErr);
        }
        return false;
    }
    return dlopen_remote_path(pid, copied.c_str(), errOut, errCap);
}

#endif
