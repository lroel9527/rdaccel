#include <gtest/gtest.h>

#include <vector>

#include "capture/dirty_rects.h"

using rdacore::Rect;
using rdacore::capture::AppendDirtyRect;

namespace {

bool Covers(const std::vector<Rect>& rects, int32_t x, int32_t y) {
  for (const Rect& r : rects) {
    if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return true;
  }
  return false;
}

TEST(DirtyRects, AtScaleOneARectIsItself) {
  std::vector<Rect> out;
  AppendDirtyRect(10, 20, 30, 40, 1.0, 0, 0, 1920, 1080, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].x, 10);
  EXPECT_EQ(out[0].y, 20);
  EXPECT_EQ(out[0].w, 30);
  EXPECT_EQ(out[0].h, 40);
}

// What macOS 27 reports on a 2816 x 1762 picture of a 1408 x 881 point
// display: the dirty rect is in pixels. The menu bar (the top 60 pixels) and
// a clock at the right edge must be covered.
TEST(DirtyRects, PixelsOnAHiDpiDisplayStayWhereTheyAre) {
  std::vector<Rect> out;
  AppendDirtyRect(2600, 0, 216, 60, 2.0, 0, 0, 2816, 1762, out);
  EXPECT_TRUE(Covers(out, 2700, 30));
  EXPECT_TRUE(Covers(out, 2815, 59));
}

// And if a system reports points, those are covered as well.
TEST(DirtyRects, PointsOnAHiDpiDisplayAreCoveredToo) {
  std::vector<Rect> out;
  AppendDirtyRect(1300, 0, 108, 30, 2.0, 0, 0, 2816, 1762, out);
  EXPECT_TRUE(Covers(out, 2700, 30));  // read as points
  EXPECT_TRUE(Covers(out, 1350, 15));  // read as pixels
  // A content area that does not start at the origin.
  out.clear();
  AppendDirtyRect(110, 60, 10, 10, 2.0, 100, 50, 2816, 1762, out);
  EXPECT_TRUE(Covers(out, 25, 25));
}

TEST(DirtyRects, StaysInsideThePictureAndDropsEmptyRects) {
  std::vector<Rect> out;
  AppendDirtyRect(0, 60, 2816, 1524, 2.0, 0, 0, 2816, 1762, out);
  for (const Rect& r : out) {
    EXPECT_GE(r.x, 0);
    EXPECT_GE(r.y, 0);
    EXPECT_LE(r.x + r.w, 2816);
    EXPECT_LE(r.y + r.h, 1762);
    EXPECT_GT(r.w, 0);
    EXPECT_GT(r.h, 0);
  }
  EXPECT_TRUE(Covers(out, 0, 60));       // as pixels: from row 60
  EXPECT_TRUE(Covers(out, 2815, 1583));
  out.clear();
  AppendDirtyRect(0, 0, 0, 0, 2.0, 0, 0, 2816, 1762, out);        // what an untouched frame carries
  AppendDirtyRect(5000, 5000, 10, 10, 2.0, 0, 0, 2816, 1762, out);  // outside
  AppendDirtyRect(-50, -50, 10, 10, 1.0, 0, 0, 2816, 1762, out);
  EXPECT_TRUE(out.empty());
}

}  // namespace
