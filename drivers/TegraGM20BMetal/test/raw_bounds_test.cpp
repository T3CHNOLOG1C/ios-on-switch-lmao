// SPDX-License-Identifier: MIT
// raw/runtime/raw_bounds.h on the host: the box and the rectangle of a draw of Core Animation's vertex transform, over
// synthetic quads, triangles and clipped vertices. raw/test/boundsoracle.mm checks the same against a GPU's coverage.
#include <gtest/gtest.h>

#include <vector>

#include "../raw/runtime/raw_bounds.h"

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
const raw::PixelBox kTarget = {0, 0, kWidth, kHeight};
const raw::WindowViewport kViewport = {0, 0, kWidth, kHeight};

// Core Animation's projection of a layer's pixels: x right and y down over the viewport
const float kPixels[16] = {2.0f / kWidth, 0, 0, 0, 0, -2.0f / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};

struct Corner {
    float x, y;
};

std::vector<float> positions(const std::vector<Corner> &corners, float z = 0.5f, float w = 1)
{
    std::vector<float> out;
    for (const Corner &c : corners)
        out.insert(out.end(), {c.x * w, c.y * w, z * w, w});
    return out;
}

bool bounds(raw::DrawBounds &b, const std::vector<float> &p, uint32_t components = 4)
{
    const raw::PositionStream s = {reinterpret_cast<const uint8_t *>(p.data()), components * 4, 0, components};
    bool ok = true;
    for (uint32_t v = 0; v < p.size() / components; v++)
        ok &= b.add(s, v);
    return ok;
}

void expectBox(const raw::PixelBox &b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    EXPECT_EQ(b.x0, x0);
    EXPECT_EQ(b.y0, y0);
    EXPECT_EQ(b.x1, x1);
    EXPECT_EQ(b.y1, y1);
}

const Corner kA = {10, 30}, kB = {20, 30}, kC = {10, 50}, kD = {20, 50};

TEST(RawBounds, PixelAlignedStripIsItsRectangleExactly)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD})));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
    expectBox(possible, 10, 30, 20, 50);
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(RawBounds, FoldedStripIsNoRectangle)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kD, kC})));
    raw::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(RawBounds, TwoTriangleListsAlongEitherDiagonal)
{
    raw::PixelBox certain, possible;
    for (const std::vector<Corner> &list :
         {std::vector<Corner>{kA, kB, kC, kB, kD, kC}, std::vector<Corner>{kA, kB, kD, kA, kD, kC}}) {
        raw::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions(list)));
        ASSERT_TRUE(b.rectangle(true, certain, possible));
        expectBox(certain, 10, 30, 20, 50);
    }
    raw::DrawBounds twice(kPixels, kViewport);
    ASSERT_TRUE(bounds(twice, positions({kA, kB, kC, kA, kB, kC})));
    EXPECT_FALSE(twice.rectangle(true, certain, possible));
}

TEST(RawBounds, HalfPixelCornersLeaveTheirEdgesInDoubt)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{10.5f, 30}, {20.5f, 30}, {10.5f, 50}, {20.5f, 50}})));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 11, 30, 20, 50);
    expectBox(possible, 10, 30, 21, 50);
}

TEST(RawBounds, TriangleBoxHoldsItsVertices)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{100.25f, 7}, {300, 200.75f}, {50, 400}})));
    expectBox(b.pixels(kTarget), 49, 6, 301, 401);
    raw::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
}

TEST(RawBounds, NormalizedYGrowsUpFromTheViewportsTop)
{
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    raw::DrawBounds b(identity, {100, 40, 200, 100});
    ASSERT_TRUE(bounds(b, positions({{-1, 1}, {-0.5f, 1}, {-1, 0.6f}})));
    expectBox(b.pixels(kTarget), 99, 39, 151, 61);
}

TEST(RawBounds, FloatTwoPositionsTakeZeroAndOne)
{
    raw::DrawBounds b(kPixels, kViewport);
    const std::vector<float> p = {10, 30, 20, 30, 10, 50, 20, 50};
    ASSERT_TRUE(bounds(b, p, 2));
    raw::PixelBox certain, possible;
    EXPECT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(RawBounds, PerspectiveScalesByW)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, 0.5f, 4)));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(RawBounds, AVertexAtOrBehindWZeroLeavesNoBox)
{
    raw::DrawBounds behind(kPixels, kViewport);
    EXPECT_FALSE(bounds(behind, positions({kA, kB, kC}, 0.5f, -1)));
    raw::DrawBounds at(kPixels, kViewport);
    EXPECT_FALSE(bounds(at, {10, 30, 0.5f, 0}));
    raw::DrawBounds nan(kPixels, kViewport);
    EXPECT_FALSE(bounds(nan, {NAN, 30, 0.5f, 1}));
}

TEST(RawBounds, DepthClippedIsABoxButNoRectangle)
{
    raw::PixelBox certain, possible;
    for (const float z : {-0.25f, 1.25f}) {
        raw::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, z)));
        EXPECT_FALSE(b.rectangle(false, certain, possible));
        expectBox(b.pixels(kTarget), 9, 29, 21, 51);
    }
}

TEST(RawBounds, OffTargetClampsToTheLimit)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{-500, -20}, {1500, -20}, {-500, 900}, {1500, 900}})));
    expectBox(b.pixels(kTarget), 0, 0, kWidth, kHeight);
    expectBox(b.pixels({100, 200, 300, 400}), 100, 200, 300, 400);
}

}  // namespace
