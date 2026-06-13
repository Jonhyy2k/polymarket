#pragma once

#include "types.hpp"

void apply_process_runtime_tuning(const Config& config);
void apply_thread_runtime_tuning(const char* role, int cpu, int priority, size_t prefault_stack_kb);
