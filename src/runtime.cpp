#include "runtime.hpp"

#include <cstdio>

#ifdef __linux__
#include <alloca.h>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace {

#ifdef __linux__
void prefault_stack(size_t stack_kb) {
    if (stack_kb == 0) return;

    const size_t bytes = stack_kb * 1024;
    volatile char* stack = static_cast<volatile char*>(alloca(bytes));
    for (size_t i = 0; i < bytes; i += 4096) {
        stack[i] = 0;
    }
    stack[bytes - 1] = 0;
}
#endif

}  // namespace

void apply_process_runtime_tuning(const Config& config) {
#ifdef __linux__
    if (config.lock_memory) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
            std::printf("[RUNTIME] Locked process memory\n");
        } else {
            std::fprintf(stderr, "[RUNTIME] mlockall failed: %s\n", std::strerror(errno));
        }
    }
#else
    (void)config;
#endif
}

void apply_thread_runtime_tuning(const char* role, int cpu, int priority, size_t prefault_stack_kb) {
#ifdef __linux__
    prefault_stack(prefault_stack_kb);

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);

        const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        if (rc == 0) {
            std::printf("[RUNTIME] Pinned %s thread to CPU %d\n", role, cpu);
        } else {
            std::fprintf(stderr, "[RUNTIME] Failed to pin %s thread to CPU %d: %s\n",
                         role, cpu, std::strerror(rc));
        }
    }

    if (priority > 0) {
        sched_param param{};
        param.sched_priority = priority;
        if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
            std::printf("[RUNTIME] Enabled %s thread SCHED_FIFO priority %d\n", role, priority);
        } else {
            std::fprintf(stderr, "[RUNTIME] Failed to set %s thread SCHED_FIFO priority %d: %s\n",
                         role, priority, std::strerror(errno));
        }
    }
#else
    (void)role;
    (void)cpu;
    (void)priority;
    (void)prefault_stack_kb;
#endif
}
