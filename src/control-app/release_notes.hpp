#pragma once

#include <string>
#include <string_view>

namespace soundfork::control {
inline std::wstring_view selected_release_notes(std::wstring_view body) noexcept {
  constexpr std::wstring_view start = L"<!-- soundfork-notes:start -->";
  constexpr std::wstring_view end = L"<!-- soundfork-notes:end -->";
  const auto first = body.find(start);
  if (first == body.npos) return body;
  const auto after = first + start.size();
  const auto last = body.find(end, after);
  if (last == body.npos) return body;
  body = body.substr(after, last - after);
  while (!body.empty() && (body.front() == L'\r' || body.front() == L'\n')) body.remove_prefix(1);
  while (!body.empty() && (body.back() == L'\r' || body.back() == L'\n')) body.remove_suffix(1);
  return body;
}

inline void append_rtf_text(std::string& out, std::wstring_view text) {
  for (const auto c : text) {
    if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
    else if (c == L'\t') out += "\\tab ";
    else if (c >= 32 && c < 127) out += static_cast<char>(c);
    else if (c >= 128) { out += "\\u"; out += std::to_string(c <= 32767 ? static_cast<int>(c) : static_cast<int>(c) - 65536); out += '?'; }
  }
}

// Deliberately small Markdown subset. All source characters are escaped before
// entering RTF: release content cannot inject RTF controls, objects or links.
inline std::string release_notes_rtf(std::wstring_view body) {
  body = selected_release_notes(body);
  std::string out = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0 Segoe UI;}{\\f1 Consolas;}}\\f0\\fs20 ";
  while (!body.empty()) {
    const auto newline = body.find(L'\n');
    auto line = body.substr(0, newline);
    if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
    std::size_t heading = 0;
    while (heading < line.size() && line[heading] == L'#') ++heading;
    const bool title = heading >= 1 && heading <= 6 && heading < line.size() && line[heading] == L' ';
    out += "\\pard\\sa100\\fs20\\b0 ";
    if (title) {
      line.remove_prefix(heading + 1);
      out += heading == 1 ? "\\sb160\\fs28\\b " : "\\sb120\\fs23\\b ";
    } else if (line.starts_with(L"- ") || line.starts_with(L"* ") || line.starts_with(L"+ ")) {
      line.remove_prefix(2); out += "\\li240\\fi-180 \\u8226?\\tab ";
    }
    while (!line.empty()) {
      const bool bold = line.starts_with(L"**");
      const bool code = line.starts_with(L"`");
      const std::size_t delimiter = bold ? 2 : 1;
      const auto end = (bold || code) ? line.find(bold ? L"**" : L"`", delimiter) : line.npos;
      if (end != line.npos) {
        out += bold ? "{\\b " : "{\\f1 ";
        append_rtf_text(out, line.substr(delimiter, end - delimiter)); out += '}';
        line.remove_prefix(end + delimiter);
      } else { append_rtf_text(out, line.substr(0, 1)); line.remove_prefix(1); }
    }
    out += "\\par ";
    if (newline == body.npos) break;
    body.remove_prefix(newline + 1);
  }
  out += '}'; return out;
}
}  // namespace soundfork::control
