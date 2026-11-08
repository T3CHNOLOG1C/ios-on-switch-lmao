// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// The textures of every level the encoder at `at` uses, current in m2v first (a blit encoder of their own before it): level 0
// copied from the raw texture where it was written since, the levels generated where the raw path generated them since (both
// from level 0, as m2v's own generateMipmaps makes them). A generateMipmaps needs level 0 only.
void mipSync(RMCommandBuffer *cb, id<MTLCommandBuffer> m, size_t at, size_t to)
{
    std::vector<std::pair<RMTexture *, bool>> need;
    auto add = [&](void *o, bool levels) {
        if (!chained(o))
            return;
        RMTexture *t = (__bridge RMTexture *)o;
        for (auto &n : need)
            if (n.first == t) {
                n.second = n.second || levels;
                return;
            }
        need.push_back({t, levels});
    };
    // A pass into a later level needs m2v's levels current first, or the next sync would write over what it renders.
    if (cb->_tape[at].op == OpRender) {
        MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)cb->_tape[at].obj;
        for (unsigned j = 0; j < 8; j++)
            if (pass.colorAttachments[j].level)
                add((__bridge void *)pass.colorAttachments[j].texture, true);
    }
    for (size_t k = at + 1; k < to && cb->_tape[k].op != OpEnd; k++) {
        const Cmd &c = cb->_tape[k];
        switch (c.op) {
        case OpTexture:
            if (c.obj && [(__bridge id)c.obj isKindOfClass:[RMTexture class]]) {
                RMTexture *t = (__bridge RMTexture *)c.obj;
                add(t->_kind == RMTextureView && t->_parent ? (__bridge void *)t->_parent : c.obj, true);
            }
            break;
        case OpMipmaps:
            add(c.obj, false);
            break;
        case OpCopyTexture:
        case OpCopyTextureToBuffer:
            if (extra(cb, c.x + 1))
                add(c.obj, true);  // a level below the first, read
            if (c.op == OpCopyTexture && extra(cb, c.x + 9))
                add(c.obj2, true);  // one written: the rest of it stays what m2v's levels hold
            break;
        case OpCopyBufferToTexture:
            if (extra(cb, c.x + 7))
                add(c.obj2, true);
            break;
        default:
            break;
        }
    }
    bool any = false;
    for (auto &n : need)
        any = any || !n.first->_mipL0 || (n.second && !n.first->_mipLevels);
    if (!any)
        return;
    id<MTLBlitCommandEncoder> b = [m blitCommandEncoder];
    for (auto &n : need) {
        RMTexture *t = n.first;
        id<MTLTexture> all = [t rmMipTwin];
        if (!all)
            continue;
        if (!t->_mipL0) {
            [b copyFromTexture:(id<MTLTexture>)[t rmTwin]
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(t->_raw.width, t->_raw.height, 1)
                        toTexture:all
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            t->_mipL0 = true;
        }
        if (n.second && !t->_mipLevels) {
            // Levels the raw path rendered are not what generating gives: they come from the mirrors mirrorsForM2v filled
            // just before this command buffer. Otherwise the chain's levels are generated ones, and generating is exact.
            bool copied = false;
            if (t->_raw.chain->rawLevels && t->_mirrored) {
                copied = true;
                for (uint32_t l = 1; l < t->_raw.chain->layout.levels; l++) {
                    RMTexture *mirror = [t rmMirror:l];
                    id<MTLTexture> from = mirror ? (id<MTLTexture>)[mirror rmTwin] : nil;
                    if (!from) {
                        copied = false;
                        break;
                    }
                    const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[l];
                    [b copyFromTexture:from
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(lv.width, lv.height, 1)
                                toTexture:all
                         destinationSlice:0
                         destinationLevel:l
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                }
            }
            if (!copied)
                [b generateMipmapsForTexture:all];
            t->_mipLevels = true;
            t->_mirrored = false;
        }
    }
    [b endEncoding];
}

// m2v wrote level `level` of a mipmapped texture: level 0 (its in-place twin: the raw texture's pages) puts the chain's copy and
// m2v's own behind it; another level (m2v's texture of every level) goes to the level's mirror once the encoder ends
// (mirrorWrites). With no mirror the chain's levels are marked behind m2v's and are generated from level 0.

void mipWritten(void *o, uint64_t level, MirrorWrites *toMirrors)
{
    if (!chained(o))
        return;
    RMTexture *t = (__bridge RMTexture *)o;
    if (level) {
        if (toMirrors && level < t->_raw.chain->layout.levels && [t rmMirror:level])
            toMirrors->push_back({t, uint32_t(level)});
        else
            t->_raw.chain->levelsFresh = false;
    } else {
        t->_raw.chain->l0Fresh = false;
        t->_mipL0 = false;
    }
}

// Copies the levels the encoder that just ended wrote into their mirrors, in a blit encoder after it.
void mirrorWrites(id<MTLCommandBuffer> m, MirrorWrites &writes)
{
    if (writes.empty())
        return;
    id<MTLBlitCommandEncoder> b = [m blitCommandEncoder];
    for (const auto &w : writes) {
        RMTexture *t = w.first, *mirror = [t rmMirror:w.second];
        id<MTLTexture> to = mirror ? (id<MTLTexture>)[mirror rmTwin] : nil, all = [t rmMipTwin];
        if (!to || !all) {
            t->_raw.chain->levelsFresh = false;
            continue;
        }
        const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[w.second];
        [b copyFromTexture:all
                  sourceSlice:0
                  sourceLevel:w.second
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(lv.width, lv.height, 1)
                    toTexture:to
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        t->_raw.chain->mirrorLevels |= uint16_t(1u << w.second);
        t->_mirrored = false;
    }
    [b endEncoding];
    writes.clear();
}

// Before an m2v command buffer that uses the later levels of a mipmapped texture the raw path rendered levels of (so they are
// not what m2v's generateMipmaps gives): every later level out of the chain into its mirror, on the raw channel, ordered
// before the m2v work on the GPU (rawSinceM2v: its submission acquires on this one); mipSync copies them on. In CPU order
// (the kernel refused Order) m2v generates them from level 0 instead.
void mirrorsForM2v(RMCommandBuffer *cb, size_t from, size_t to)
{
    RawState &r = *gRM;
    if (!cb->_chains || !r.gpuOrder)
        return;
    std::vector<RMTexture *> need;
    auto add = [&](void *o) {
        if (!chained(o))
            return;
        RMTexture *t = (__bridge RMTexture *)o;
        const raw::Texture::Chain &c = *t->_raw.chain;
        if (t->_mipLevels || t->_mirrored || !c.rawLevels || !c.levelsFresh || c.mirrorLevels)
            return;
        if (std::find(need.begin(), need.end(), t) == need.end())
            need.push_back(t);
    };
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            for (unsigned j = 0; j < 8; j++)
                if (pass.colorAttachments[j].level)
                    add((__bridge void *)pass.colorAttachments[j].texture);
            break;
        }
        case OpTexture:
            if (c.obj && [(__bridge id)c.obj isKindOfClass:[RMTexture class]]) {
                RMTexture *t = (__bridge RMTexture *)c.obj;
                add(t->_kind == RMTextureView && t->_parent ? (__bridge void *)t->_parent : c.obj);
            }
            break;
        case OpCopyTexture:
        case OpCopyTextureToBuffer:
            if (extra(cb, c.x + 1))
                add(c.obj);
            if (c.op == OpCopyTexture && extra(cb, c.x + 9))
                add(c.obj2);
            break;
        case OpCopyBufferToTexture:
            if (extra(cb, c.x + 7))
                add(c.obj2);
            break;
        default:
            break;
        }
    }
    for (auto it = need.begin(); it != need.end();) {
        bool all = true;
        for (uint32_t l = 1; all && l < (*it)->_raw.chain->layout.levels; l++)
            all = [*it rmMirror:l] != nil;
        it = all ? std::next(it) : need.erase(it);
    }
    if (need.empty())
        return;
    if (r.m2vSinceRaw) {  // m2v work before may still read the mirrors
        uint32_t waits = 0;
        const int kr = r.raw->orderAfterOthers(waits);
        if (kr != KERN_SUCCESS) {
            orderRefused("raw mirrors after m2v", kr);
            return;
        }
        r.m2vSinceRaw = false;
        r.stats.acquires += waits;
    }
    raw::Device &d = *r.raw;
    raw::CommandBuffer *rc = d.begin();
    if (!rc)
        return;
    raw::BlitEncoder *be = rc->blit();
    uint64_t levels = 0;
    for (RMTexture *t : need)
        for (uint32_t l = 1; l < t->_raw.chain->layout.levels; l++) {
            RMTexture *mirror = [t rmMirror:l];
            const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[l];
            be->copyFromLevel(t->_raw, l, 0, 0, mirror->_raw, 0, 0, lv.width, lv.height);
            [cb retain:mirror];  // until the m2v work that reads it completes, which runs after this copy
            levels++;
        }
    be->end();
    uint32_t seq = 0;
    if (!rc->commit(seq)) {
        rc->abandon();
        once(@"the kernel refused the mirror copies of mipmapped levels: m2v generates them from level 0");
        return;
    }
    r.rawSinceM2v = true;
    d.stats.mirrorsOut += levels;
    for (RMTexture *t : need)
        t->_mirrored = true;
}
