/*
 * Copyright 2025-2026 AxionOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct proc;

namespace {

std::atomic<int64_t> gRamPlusSwapKb{0};
constexpr char kSwapPath[] = "/data/system/axion_ram_plus/swapfile";

int64_t ReadRamPlusSwapKb() {
    FILE* swaps = fopen("/proc/swaps", "re");
    if (swaps == nullptr) {
        return 0;
    }

    char line[512];
    if (fgets(line, sizeof(line), swaps) == nullptr) {
        fclose(swaps);
        return 0;
    }

    char path[256];
    char type[32];
    long long size_kb;
    long long used_kb;
    int priority;
    while (fscanf(swaps, "%255s %31s %lld %lld %d", path, type, &size_kb, &used_kb, &priority) ==
           5) {
        if (strcmp(path, kSwapPath) == 0) {
            fclose(swaps);
            return std::max<long long>(0, size_kb);
        }
    }

    fclose(swaps);
    return 0;
}

}

extern "C" bool lmkd_update_props_hook() {
    gRamPlusSwapKb.store(ReadRamPlusSwapKb(), std::memory_order_relaxed);
    return true;
}

extern "C" bool lmkd_init_hook() {
    return true;
}

extern "C" int lmkd_free_memory_before_kill_hook(struct proc*, int, int, int) {
    return 0;
}

extern "C" void lmkd_no_kill_candidates_hook() {}

extern "C" int64_t lmkd_adjust_free_swap_hook(int64_t free_swap, int64_t easy_available,
                                              int64_t compression_ratio,
                                              int64_t compression_ratio_div,
                                              int64_t adjusted_free_swap) {
    const int64_t disk_total = std::max<int64_t>(0, gRamPlusSwapKb.load(std::memory_order_relaxed));
    if (disk_total == 0 || compression_ratio <= 0 || compression_ratio_div <= 0) {
        return adjusted_free_swap;
    }

    const int64_t disk_free = std::min(free_swap, disk_total);
    const int64_t zram_free = std::max<int64_t>(0, free_swap - disk_total);
    return disk_free +
           std::min(zram_free, easy_available * compression_ratio / compression_ratio_div);
}
