#include "dcache_recorder.h"

#include <cstring>
#include <vector>

namespace {
std::vector<switch_dcache_event> &events()
{
    static std::vector<switch_dcache_event> recorded;
    return recorded;
}
}  // namespace

void switch_dcache_record(const char *op, uintptr_t address)
{
    switch_dcache_event event{};
    std::strncpy(event.op, op, sizeof(event.op) - 1);
    event.address = address;
    events().push_back(event);
}

size_t switch_dcache_recorded(const switch_dcache_event **recorded)
{
    *recorded = events().data();
    return events().size();
}

void switch_dcache_forget(void)
{
    events().clear();
}
