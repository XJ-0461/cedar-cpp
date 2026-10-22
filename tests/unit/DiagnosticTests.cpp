#include <cedar/Diagnostic.hpp>

#include <gtest/gtest.h>

namespace
{

using cedar::LocateOffset;
using cedar::SourceLocation;

TEST(DiagnosticTests, LocatesOffsetsOnTheFirstLine)
{
    EXPECT_EQ(LocateOffset("permit", 0), (SourceLocation{.line = 1, .column = 1}));
    EXPECT_EQ(LocateOffset("permit", 3), (SourceLocation{.line = 1, .column = 4}));
}

TEST(DiagnosticTests, CountsLinesAtNewlines)
{
    constexpr std::string_view text = "ab\ncd\n\nef";
    EXPECT_EQ(LocateOffset(text, 2), (SourceLocation{.line = 1, .column = 3}));
    EXPECT_EQ(LocateOffset(text, 3), (SourceLocation{.line = 2, .column = 1}));
    EXPECT_EQ(LocateOffset(text, 6), (SourceLocation{.line = 3, .column = 1}));
    EXPECT_EQ(LocateOffset(text, 8), (SourceLocation{.line = 4, .column = 2}));
}

TEST(DiagnosticTests, TreatsCarriageReturnLineEndsAsOneLineBreak)
{
    constexpr std::string_view text = "ab\r\ncd";
    EXPECT_EQ(LocateOffset(text, 4), (SourceLocation{.line = 2, .column = 1}));
    EXPECT_EQ(LocateOffset(text, 5), (SourceLocation{.line = 2, .column = 2}));
}

TEST(DiagnosticTests, CountsColumnsInScalarValuesNotBytes)
{
    // "é" is two bytes and "€" is three; the byte after each is one column on.
    constexpr std::string_view text = "\"\xC3\xA9\xE2\x82\xAC\"";
    EXPECT_EQ(LocateOffset(text, 1), (SourceLocation{.line = 1, .column = 2}));
    EXPECT_EQ(LocateOffset(text, 3), (SourceLocation{.line = 1, .column = 3}));
    EXPECT_EQ(LocateOffset(text, 6), (SourceLocation{.line = 1, .column = 4}));
}

TEST(DiagnosticTests, ClampsOffsetsPastTheEnd)
{
    EXPECT_EQ(LocateOffset("ab\nc", 100), (SourceLocation{.line = 2, .column = 2}));
    EXPECT_EQ(LocateOffset("", 5), (SourceLocation{.line = 1, .column = 1}));
}

} // namespace
