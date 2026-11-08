// SPDX-License-Identifier: MIT
// A state's per-state 3D registers computed by the raw path from its descriptor and its two programs, the registers NVK emits for
// m2v's translation of the state (a record's blob, raw_record.h), with no NVK pipeline or capture. test/emitcheck holds them
// to every record of a pack. The vertex attributes and streams (SET_VERTEX_ATTRIBUTE_A, SET_VERTEX_STREAM_A_FORMAT) need the
// vertex function's attribute locations and are not here yet.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace raw {

// What a program's compile tells (NAK's shader info): its header, its register count and, for a fragment program, whether
// it asks for early fragment tests
struct EmitProgram {
    const uint8_t *header = nullptr;  // the 80-byte shader program header
    uint32_t gprs = 0;
    bool earlyFragmentTests = false;
};

struct EmitRegister {
    uint32_t method, value;
    // false: the hardware does not read it for this state (a blend of a target it writes nothing to, or one whose blending
    // is off), so any value draws the same
    bool read;
};

// `keyFields`: the state key's fields before " | v" (Pack::string(keyString), RuntimeRecord::keyString). False: a field
// it can't read (`why`)
bool emitState(const std::string &keyFields, const EmitProgram &vertex, const EmitProgram &fragment,
               std::vector<EmitRegister> &out, std::string &why);

}  // namespace raw
