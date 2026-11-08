// SPDX-License-Identifier: MIT
// A Metal call the raw facade records as an invocation and replays on m2v at commit (RawMetal.mm forwardInvocation:): what its
// pointer arguments point to, and the copy of it the invocation keeps.
#pragma once
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <cstring>

namespace raw {

// The type a recorded argument's encoding names, past its qualifiers (const, in, out, ...)
inline const char *argumentType(NSMethodSignature *sig, NSUInteger i)
{
    const char *t = [sig getArgumentTypeAtIndex:i];
    while (*t == 'r' || *t == 'n' || *t == 'N' || *t == 'o' || *t == 'O' || *t == 'R' || *t == 'V')
        t++;
    return t;
}

inline bool countType(const char *t)
{
    return t[0] == 'Q' || t[0] == 'L' || t[0] == 'I';
}

// The elements a pointer argument of a Metal call points to: the length of the range after it (set...s:withRange:), else the
// count after it (setBytes:length:, setViewports:count:, useResources:count:), else the count just before it
// (setVertexAmplificationCount:viewMappings:); 0 where none does
inline NSUInteger pointedElements(NSInvocation *inv, NSUInteger i)
{
    NSMethodSignature *sig = inv.methodSignature;
    NSUInteger n = 0;
    for (NSUInteger k = i + 1; k < sig.numberOfArguments; k++) {
        const char *u = argumentType(sig, k);
        if (!strncmp(u, "{_NSRange=", 10)) {
            NSRange r = {};
            [inv getArgument:&r atIndex:k];
            return r.length;
        }
        if (countType(u)) {
            [inv getArgument:&n atIndex:k];
            return n;
        }
    }
    if (i > 2 && countType(argumentType(sig, i - 1)))
        [inv getArgument:&n atIndex:i - 1];
    return n;
}

// A call recorded to be replayed on m2v at commit: its pointer arguments' data (bytes, structs, offsets, object arrays) copied
// into storage the invocation keeps, and the objects of its arrays retained (retainArguments keeps objects, C strings and blocks
// alone; the rest still pointed into the caller's stack). False: a pointer argument whose extent no count names.
inline bool keepPointedArguments(NSInvocation *inv)
{
    NSMethodSignature *sig = inv.methodSignature;
    bool known = true;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = argumentType(sig, i);
        if (t[0] != '^' && t[0] != '*')
            continue;
        const void *from = nullptr;
        [inv getArgument:&from atIndex:i];
        if (!from || t[0] == '*')
            continue;
        const NSUInteger n = pointedElements(inv, i);
        if (!n) {
            known = false;
            continue;
        }
        NSUInteger size = 1, align = 1;
        if (t[1] != 'v')
            NSGetSizeAndAlignment(t + 1, &size, &align);
        NSMutableData *copy = [NSMutableData dataWithBytes:from length:n * size];
        id keep = copy;
        if (t[1] == '@') {
            NSMutableArray *objects = [NSMutableArray arrayWithObject:copy];
            id __unsafe_unretained const *array = (id __unsafe_unretained const *)from;
            for (NSUInteger k = 0; k < n; k++)
                if (array[k])
                    [objects addObject:array[k]];
            keep = objects;
        }
        objc_setAssociatedObject(inv, (__bridge const void *)copy, keep, OBJC_ASSOCIATION_RETAIN);
        void *to = copy.mutableBytes;
        [inv setArgument:&to atIndex:i];
    }
    return known;
}

}  // namespace raw
