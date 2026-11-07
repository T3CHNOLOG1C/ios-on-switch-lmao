// SPDX-License-Identifier: MIT
// recordcheck: raw::makeRecord (the runtime's completion of a state the pack lacks) against the pack mkpack.py made from the same
// probe runs. Every state the pack holds a record for must come out byte for byte the same (its blob, set-0 entries, push words,
// vertex stream, set size, flags, key fields and both programs' heap slots, and no registers outside the pack's varying ones);
// every state it set aside must be refused, a storage-form state among them.
// usage: recordcheck <bucket dir (probe{256,16,4096}.{out,log}, programs.bin)> <pack.bin>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../runtime/raw_pack.h"
#include "../runtime/raw_record.h"

namespace {

struct Run {
    std::vector<std::pair<uint32_t, std::string>> probes;  // PROBE <index> <key>
    std::vector<nvdec::ProbeSubmission> submissions;       // the last ones: a probe's each
    std::map<uint32_t, uint32_t> context;                  // S regs 0
};

std::vector<std::string> fields(const std::string &line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string f; in >> f;)
        out.push_back(f);
    return out;
}

uint32_t hex(const std::string &s)
{
    return uint32_t(strtoull(s.c_str(), nullptr, 16));
}

std::vector<uint32_t> words(const std::vector<std::string> &f, size_t from)
{
    std::vector<uint32_t> out;
    for (size_t i = from; i < f.size(); i++)
        out.push_back(hex(f[i]));
    return out;
}

std::map<uint32_t, uint32_t> pairs(const std::vector<std::string> &f, size_t from)
{
    std::map<uint32_t, uint32_t> out;
    for (size_t i = from; i < f.size(); i++)
        if (const size_t eq = f[i].find('='); eq != std::string::npos)
            out[hex(f[i].substr(0, eq))] = hex(f[i].substr(eq + 1));
    return out;
}

// probelog.py _record_binding and _record_state
void recordLine(nvdec::ProbeRecord &r, const std::vector<std::string> &f)
{
    const std::string &t = f[1];
    if (t == "regs")
        r.regs = pairs(f, 2);
    else if (t == "root")
        r.root = words(f, 3);
    else if (t == "set0")
        r.set0 = words(f, 3);
    else if (t.rfind("ptr0.", 0) == 0)
        r.set0Pointers[uint32_t(atoi(t.c_str() + 5))] = words(f, 3);
    else if (t == "tic") {
        nvdec::ProbeRecord::Header &h = r.tic[uint32_t(atoi(f[2].c_str()))];
        h.name = f[3];
        for (size_t i = 0; i < 8; i++)
            h.w[i] = hex(f[4 + i]);
    } else if (t == "tsc") {
        std::array<uint32_t, 8> &w = r.tsc[uint32_t(atoi(f[2].c_str()))];
        for (size_t i = 0; i < 8; i++)
            w[i] = hex(f[3 + i]);
    } else if (t == "vbinfo")
        r.streams[uint32_t(atoi(f[2].c_str()))].format = hex(f[5]);
    else if (t.rfind("vb", 0) == 0)
        r.streams[uint32_t(atoi(t.c_str() + 2))].words = words(f, 3);
    else if (t == "prog")
        r.programs[atoi(f[2].c_str())] = {f[3], f[5] == "known"};
    else if (t == "pushw")
        for (const auto &kv : pairs(f, 2))
            r.pushWords.push_back(kv);
}

bool loadRun(const std::string &dir, int bytes, Run &run)
{
    std::ifstream out(dir + "/probe" + std::to_string(bytes) + ".out"),
        log(dir + "/probe" + std::to_string(bytes) + ".log");
    if (!out || !log)
        return false;
    for (std::string line; std::getline(out, line);)
        if (line.rfind("PROBE ", 0) == 0) {
            const size_t a = line.find(' ', 6);
            std::string key = line.substr(a + 1);
            while (!key.empty() && key.back() == ' ')
                key.pop_back();
            run.probes.push_back({uint32_t(atoi(line.c_str() + 6)), key});
        }
    std::vector<nvdec::ProbeSubmission> subs(1);
    nvdec::ProbeRecord *rec = nullptr;
    for (std::string line; std::getline(log, line);) {
        nvdec::ProbeSubmission &s = subs.back();
        if (line.rfind("SUBMIT", 0) == 0) {
            subs.emplace_back();
            continue;
        }
        if (line.rfind("S regs 0 ", 0) == 0)
            run.context = pairs(fields(line), 3);
        else if (line.rfind("DRAW", 0) == 0)
            s.draws++;
        else if (line.rfind("DISPATCH", 0) == 0)
            s.dispatches++;
        else if (line.rfind("CLEAR", 0) == 0)
            s.clears++;
        else if (line.rfind("  rt", 0) == 0 && line.find('=') != std::string::npos) {
            const size_t eq = line.find('=');
            s.targets[fields(line.substr(eq + 1))[0]] = uint32_t(atoi(line.c_str() + 4));
        } else if (line == "P begin") {
            s.records.emplace_back();
            rec = &s.records.back();
        } else if (line == "P end")
            rec = nullptr;
        else if (rec && line.rfind("P ", 0) == 0)
            recordLine(*rec, fields(line));
    }
    subs.pop_back();
    if (subs.size() < run.probes.size())
        return false;
    run.submissions.assign(subs.end() - run.probes.size(), subs.end());
    return true;
}

// mkprograms.py's programs.bin, by the id nvdec names a bound one with
std::map<std::string, raw::CapturedProgram> loadPrograms(const std::string &path)
{
    std::map<std::string, raw::CapturedProgram> out;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        at += 16;
        raw::CapturedProgram p;
        p.cbufTypes.assign(b.begin() + long(at), b.begin() + long(at + count));
        at += count;
        p.image.assign(b.begin() + long(at), b.begin() + long(at + 80 + code));
        p.data.assign(b.begin() + long(at + 80 + code), b.begin() + long(at + 80 + code + data));
        char id[24];
        snprintf(id, sizeof id, "%llx", (unsigned long long)raw::Pack::fnv(&b[at], 80 + code + data));
        p.id = id;
        at += 80 + code + data;
        out[p.id] = p;
    }
    return out;
}

std::string differences(const raw::Pack &pack, const raw::PackPipeline &pp, const raw::RuntimeRecord &r)
{
    std::string why;
    const raw::PackPipeline &rp = r.pipeline;
    if (pack.string(pp.keyString) != r.keyString)
        why += " key-fields";
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + pp.blob;
    if (pp.blobDwords != r.blob.size() || memcmp(blob, r.blob.data(), r.blob.size() * 4))
        why += " blob";
    if (pp.entries != rp.entries || memcmp(pp.entry, rp.entry, sizeof pp.entry))
        why += " entries";
    if (pp.pushes != rp.pushes || pp.pushMask != rp.pushMask || memcmp(pp.pushWords, rp.pushWords, sizeof pp.pushWords))
        why += " push";
    if (pp.vertexSlot != rp.vertexSlot || pp.vertexStride != rp.vertexStride)
        why += " vertex";
    if (pp.setBytes != rp.setBytes)
        why += " set-bytes";
    if (pp.flags != rp.flags)
        why += " flags";
    if (pp.keyHash != rp.keyHash)
        why += " key";
    if (!r.extras.empty() || !r.samplers.empty())
        why += " extras";
    for (int k = 0; k < 2; k++) {
        const raw::PackProgram &packed = pack.section<raw::PackProgram>(raw::PackPrograms)[k ? pp.fs : pp.vs];
        raw::PackProgram made;
        const std::vector<uint8_t> slot = raw::programSlot(r.programs[k], made);
        const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap) + packed.heapOffset;
        if (packed.bytes != made.bytes || memcmp(heap, slot.data(), slot.size()) ||
            packed.program - packed.heapOffset != made.program || packed.dataOffset != made.dataOffset ||
            packed.dataBytes != made.dataBytes || packed.cbufs != made.cbufs ||
            memcmp(packed.cbufType, made.cbufType, sizeof made.cbufType))
            why += k ? " pixel-program" : " vertex-program";
    }
    return why;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: recordcheck <bucket dir> <pack.bin>\n");
        return 2;
    }
    const std::string dir = argv[1];
    Run r256, r16, r4096;
    raw::Pack pack;
    std::string err;
    if (!loadRun(dir, 256, r256) || !loadRun(dir, 16, r16) || !loadRun(dir, 4096, r4096) ||
        !pack.load(argv[2], 0, err)) {
        fprintf(stderr, "recordcheck: %s\n", err.empty() ? "probe runs unread" : err.c_str());
        return 1;
    }
    const std::map<std::string, raw::CapturedProgram> programs = loadPrograms(dir + "/programs.bin");
    const raw::PackRules rules = raw::PackRules::of(pack, r256.context);
    auto byIndex = [](const Run &run, uint32_t index) -> const nvdec::ProbeSubmission * {
        for (size_t i = 0; i < run.probes.size(); i++)
            if (run.probes[i].first == index)
                return &run.submissions[i];
        return nullptr;
    };
    uint32_t same = 0, refused = 0, storage = 0, bad = 0;
    for (size_t i = 0; i < r256.probes.size(); i++) {
        const auto &[index, key] = r256.probes[i];
        const raw::ProbeRuns runs = {&r256.submissions[i], byIndex(r16, index), byIndex(r4096, index), index, key};
        raw::RuntimeRecord made;
        std::string why;
        const bool ok = raw::makeRecord(
            runs, rules,
            [&](const std::string &id) {
                const auto it = programs.find(id);
                return it == programs.end() ? nullptr : &it->second;
            },
            made, why);
        const raw::PackPipeline *pp = pack.find(raw::packKeyHash(key));
        const bool barrier = pp && (pp->flags & raw::PipelineBarrierForm);
        std::string verdict;
        if (pp && !barrier) {
            verdict = ok ? differences(pack, *pp, made) : " refused (" + why + ")";
            same += verdict.empty();
        } else if (barrier) {
            verdict = !ok && why.rfind("storage form", 0) == 0 ? "" : " not refused as a storage form";
            storage += verdict.empty();
        } else {
            verdict = ok ? " made, and the pack set it aside" : "";
            refused += verdict.empty();
            if (!ok)
                printf("refused %u (%s)\n", index, why.c_str());
        }
        if (!verdict.empty()) {
            bad++;
            printf("DIFFERS %u:%s: %s\n", index, verdict.c_str(), key.substr(0, 120).c_str());
        }
    }
    printf("recordcheck %s: %u states: %u records the same as the pack's, %u storage forms refused, %u set aside and "
           "refused, "
           "%u differ\n",
           dir.c_str(), uint32_t(r256.probes.size()), same, storage, refused, bad);
    return bad ? 1 : 0;
}
