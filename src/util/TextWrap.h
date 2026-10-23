#pragma once

#include <string>
#include <vector>

class GfxRenderer;

namespace textwrap {

// Break `text` into at most `maxLines` lines, each no wider than `maxWidth`
// pixels. Breaks are allowed after a space and between CJK codepoints; Latin
// words are only split when a single word is wider than the line. The last line
// is ellipsised by truncatedText() when the text does not fit.
std::vector<std::string> wrapDisplayText(const GfxRenderer& renderer, int fontId,
                                         const std::string& text, int maxWidth, int maxLines);

}  // namespace textwrap