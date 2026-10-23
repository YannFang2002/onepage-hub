#include "TextWrap.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>

namespace textwrap {
namespace {
void trimTrailingSpaces(std::string& value) {
  while (!value.empty() && value.back() == ' ') {
    value.pop_back();
  }
}

bool utf8IsCjkBreakable(uint32_t codepoint) {
  return (codepoint >= 0x2E80 && codepoint <= 0x9FFF) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
         (codepoint >= 0xFF00 && codepoint <= 0xFFEF);
}

}  // namespace
// Wraps `text` to `maxWidth` using `fontId`.
//
// GfxRenderer::wrappedText() only breaks on ASCII spaces, which is fine for the
// English UI strings it was written for but useless here: a Chinese transcription
// has no spaces at all, so the whole sentence counted as one "word" and came back
// as a single truncated line -- the text then ran off the right edge of the panel
// instead of wrapping onto the next line.
//
// This version breaks by measured pixel width and accepts the same break
// opportunities a Chinese reader expects: after a space, or between two CJK
// codepoints. Latin words are only split when a single word is wider than the
// line. Returns at most `maxLines` lines, with the last one ellipsised when the
// text does not fit (truncatedText() adds the ellipsis itself).
std::vector<std::string> wrapDisplayText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth, const int maxLines) {
  std::vector<std::string> lines;
  if (maxWidth <= 0 || maxLines <= 0 || text.empty()) {
    return lines;
  }

  // ASR output arrives as one blob and may contain newlines/tabs from the
  // gateway; collapse any whitespace run into a single space first.
  std::string source;
  source.reserve(text.size());
  for (const char c : text) {
    const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
    if (space) {
      if (!source.empty() && source.back() != ' ') {
        source.push_back(' ');
      }
    } else {
      source.push_back(c);
    }
  }
  trimTrailingSpaces(source);

  size_t pos = 0;
  while (pos < source.size() && static_cast<int>(lines.size()) < maxLines) {
    std::string line;
    size_t cursor = pos;
    size_t breakAfterSpace = 0;  // byte offset in `line` just past the last space
    size_t breakAfterCjk = 0;    // byte offset in `line` just past the last CJK char

    while (cursor < source.size()) {
      const unsigned char* start = reinterpret_cast<const unsigned char*>(source.c_str()) + cursor;
      const unsigned char* next = start;
      const uint32_t cp = utf8NextCodepoint(&next);
      const size_t cpLen = static_cast<size_t>(next - start);

      line.append(source, cursor, cpLen);
      if (renderer.getTextWidth(fontId, line.c_str()) > maxWidth) {
        line.resize(line.size() - cpLen);  // this codepoint does not fit
        break;
      }
      cursor += cpLen;
      if (cp == ' ') {
        breakAfterSpace = line.size();
      } else if (utf8IsCjkBreakable(cp)) {
        breakAfterCjk = line.size();
      }
    }

    if (cursor >= source.size()) {
      lines.push_back(line);  // the remainder fit on this line
      pos = source.size();    // mark it consumed, or the tail gets appended twice below
      break;
    }

    size_t cut = line.size();
    if (breakAfterSpace > 0) {
      cut = breakAfterSpace;  // keep the space itself out of the line
    } else if (breakAfterCjk > 0) {
      cut = breakAfterCjk;
    }
    if (cut == 0) {
      // A single codepoint is wider than the whole line: emit it anyway so the
      // loop always makes progress (truncation handles the visual overflow).
      const unsigned char* start = reinterpret_cast<const unsigned char*>(source.c_str()) + pos;
      const unsigned char* next = start;
      utf8NextCodepoint(&next);
      cut = static_cast<size_t>(next - start);
      lines.push_back(source.substr(pos, cut));
    } else {
      std::string out = line.substr(0, cut);
      trimTrailingSpaces(out);
      lines.push_back(std::move(out));
    }

    size_t advance = pos + cut;
    while (advance < source.size() && source[advance] == ' ') {
      ++advance;
    }
    if (advance <= pos) {
      advance = pos + cut;  // guarantee forward progress
    }
    pos = advance;
  }

  if (pos < source.size() && !lines.empty()) {
    // Ran out of lines with text left: ellipsise the last one.
    lines.back() = renderer.truncatedText(fontId, (lines.back() + " " + source.substr(pos)).c_str(), maxWidth);
  }
  return lines;
}

}  // namespace textwrap
