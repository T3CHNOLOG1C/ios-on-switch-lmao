// Flattened device trees for the memory gate tests (memory_gate_test, memory_ranges_test): a /reserved-memory node
// with ramoops and a zero-sized placeholder, the bootloader's reservations
// (BPMP carveout, the framebuffer, ...), and every knob a test turns (cells, ranges, nodes, the header's fields).
#ifndef IOSWITCH_TEST_FDT_BUILDER_H
#define IOSWITCH_TEST_FDT_BUILDER_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fdt {

using Bytes = std::vector<uint8_t>;

inline void Append(Bytes &out, const Bytes &more)
{
    out.insert(out.end(), more.begin(), more.end());
}

inline Bytes U32(uint32_t n)
{
    return {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
}

// Big-endian 64-bit cells, one per value.
inline Bytes Cells(std::initializer_list<uint64_t> values)
{
    Bytes out;
    for (uint64_t v : values) {
        Append(out, U32((uint32_t)(v >> 32)));
        Append(out, U32((uint32_t)v));
    }
    return out;
}

inline void PutU32(Bytes &data, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = (uint8_t)(value >> (24 - 8 * i));
}

struct Property {
    std::string key;
    Bytes value;
};

struct Node {
    std::string name;
    std::vector<Property> properties;
    std::vector<Node> children;
};

struct Reservation {
    uint64_t start, size;
};

inline std::vector<Reservation> DefaultReservations()
{
    return {{0x80000000, 0x20000}, {0xF5A00000, 0x400000}, {0x8D000000, 0x40000}};
}

inline std::vector<Node> DefaultChildren()
{
    return {{"ramoops", {{"reg", Cells({0xB0000000, 0x200000})}}, {}},
            {"framebuffer-placeholder", {{"reg", Cells({0, 0, 0, 0})}}, {}}};
}

struct Options {
    std::optional<std::vector<Reservation>> reservations;
    std::optional<std::vector<Node>> children;
    uint32_t root_cells = 2;
    uint32_t reserved_cells = 2;
    Bytes ranges;
    bool include_reserved = true;
};

namespace detail {
inline void Pad(Bytes &out, size_t length)
{
    out.insert(out.end(), (4 - length % 4) % 4, 0);
}

// A node and its subtree; property names go into `keys` in the order the Python builder appended them (children
// first: their nodes were built before their parent's).
inline Bytes Build(const Node &node, Bytes &keys)
{
    std::vector<Bytes> children;
    for (const Node &child : node.children) children.push_back(Build(child, keys));
    Bytes data = U32(1);
    data.insert(data.end(), node.name.begin(), node.name.end());
    data.push_back(0);
    Pad(data, node.name.size() + 1);
    for (const Property &p : node.properties) {
        const uint32_t offset = (uint32_t)keys.size();
        keys.insert(keys.end(), p.key.begin(), p.key.end());
        keys.push_back(0);
        Append(data, U32(3));
        Append(data, U32((uint32_t)p.value.size()));
        Append(data, U32(offset));
        Append(data, p.value);
        Pad(data, p.value.size());
    }
    for (const Bytes &child : children) Append(data, child);
    Append(data, U32(2));
    return data;
}
}  // namespace detail

inline Bytes Tree(const Options &o = {})
{
    Bytes keys;
    const Node reserved{"reserved-memory",
                        {{"#address-cells", U32(o.reserved_cells)}, {"#size-cells", U32(2)}, {"ranges", o.ranges}},
                        o.children ? *o.children : DefaultChildren()};
    // The root's properties after reserved-memory's (the Python built that node first).
    Bytes reservedNode = detail::Build(reserved, keys);
    Node root{"", {{"#address-cells", U32(o.root_cells)}, {"#size-cells", U32(2)}}, {}};
    Bytes structure = detail::Build(root, keys);
    if (o.include_reserved) structure.insert(structure.end() - 4, reservedNode.begin(), reservedNode.end());
    Append(structure, U32(9));
    Bytes reserves;
    for (const Reservation &r : o.reservations ? *o.reservations : DefaultReservations())
        Append(reserves, Cells({r.start, r.size}));
    reserves.insert(reserves.end(), 16, 0);
    const uint32_t structure_at = 40 + (uint32_t)reserves.size();
    const uint32_t strings_at = structure_at + (uint32_t)structure.size();
    const uint32_t total = strings_at + (uint32_t)keys.size();
    Bytes out;
    for (uint32_t word : {0xD00DFEEDu, total, structure_at, strings_at, 40u, 17u, 16u, 0u, (uint32_t)keys.size(),
                          (uint32_t)structure.size()})
        Append(out, U32(word));
    Append(out, reserves);
    Append(out, structure);
    Append(out, keys);
    return out;
}

}  // namespace fdt

#endif  // IOSWITCH_TEST_FDT_BUILDER_H
