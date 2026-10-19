#include "control-app/update_checker.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "soundfork_version.hpp"

namespace soundfork::control {
namespace {

constexpr wchar_t kReleaseApiHost[] = L"api.github.com";
constexpr wchar_t kReleaseApiPath[] =
    L"/repos/kjh2623jh/SoundFork/releases?per_page=10";

class InternetHandle final {
 public:
  explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
  ~InternetHandle() {
    if (value_ != nullptr) {
      WinHttpCloseHandle(value_);
    }
  }

  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  HINTERNET get() const noexcept { return value_; }

 private:
  HINTERNET value_ = nullptr;
};

struct SemanticVersion {
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::uint64_t patch = 0;
  int stage = 0;
  std::uint64_t stage_number = 0;
  bool valid = false;
};

bool parse_version_number(
    std::string_view text,
    std::size_t* offset,
    std::uint64_t* value) noexcept {
  if (*offset >= text.size() || text[*offset] < '0' || text[*offset] > '9') {
    return false;
  }
  std::uint64_t result = 0;
  while (*offset < text.size() && text[*offset] >= '0' &&
         text[*offset] <= '9') {
    const std::uint64_t digit =
        static_cast<std::uint64_t>(text[*offset] - '0');
    if (result > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
    ++*offset;
  }
  *value = result;
  return true;
}

SemanticVersion parse_semantic_version(std::string_view text) noexcept {
  SemanticVersion version;
  std::size_t offset = 0;
  if (!text.empty() && text.front() == 'v') {
    offset = 1;
  }
  if (!parse_version_number(text, &offset, &version.major) ||
      offset >= text.size() || text[offset++] != '.' ||
      !parse_version_number(text, &offset, &version.minor) ||
      offset >= text.size() || text[offset++] != '.' ||
      !parse_version_number(text, &offset, &version.patch)) {
    return version;
  }
  if (offset == text.size()) {
    version.stage = 4;
    version.valid = true;
    return version;
  }
  if (text[offset++] != '-') {
    return version;
  }
  const std::string_view suffix = text.substr(offset);
  if (suffix == "dev") {
    version.stage = 0;
    version.valid = true;
    return version;
  }
  constexpr std::pair<std::string_view, int> stages[] = {
      {"alpha.", 1}, {"beta.", 2}, {"rc.", 3}};
  for (const auto& [prefix, rank] : stages) {
    if (!suffix.starts_with(prefix)) {
      continue;
    }
    offset += prefix.size();
    if (!parse_version_number(text, &offset, &version.stage_number) ||
        offset != text.size()) {
      return {};
    }
    version.stage = rank;
    version.valid = true;
    return version;
  }
  return version;
}

bool version_is_newer(
    const SemanticVersion& candidate,
    const SemanticVersion& current) noexcept {
  if (!candidate.valid || !current.valid) {
    return false;
  }
  if (candidate.major != current.major) {
    return candidate.major > current.major;
  }
  if (candidate.minor != current.minor) {
    return candidate.minor > current.minor;
  }
  if (candidate.patch != current.patch) {
    return candidate.patch > current.patch;
  }
  if (candidate.stage != current.stage) {
    return candidate.stage > current.stage;
  }
  return candidate.stage_number > current.stage_number;
}

std::string latest_release_tag(std::string_view json) {
  constexpr std::string_view key = "\"tag_name\"";
  SemanticVersion best;
  std::string best_tag;
  std::size_t search_from = 0;
  while (search_from < json.size()) {
    const std::size_t key_at = json.find(key, search_from);
    if (key_at == std::string_view::npos) {
      break;
    }
    search_from = key_at + key.size();
    if (key_at > 0 && json[key_at - 1] == '\\') {
      continue;
    }
    const std::size_t colon = json.find(':', search_from);
    if (colon == std::string_view::npos) {
      break;
    }
    const std::size_t quote = json.find('"', colon + 1);
    if (quote == std::string_view::npos) {
      break;
    }
    const std::size_t end = json.find('"', quote + 1);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string tag(json.substr(quote + 1, end - quote - 1));
    const SemanticVersion parsed = parse_semantic_version(tag);
    if (parsed.valid && (!best.valid || version_is_newer(parsed, best))) {
      best = parsed;
      best_tag = tag;
    }
    search_from = end + 1;
  }
  return best_tag;
}

UpdateResult check_for_updates_impl() {
  UpdateResult result;
  InternetHandle session(WinHttpOpen(
      L"SoundFork/" SOUNDFORK_VERSION_WIDE,
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0));
  if (session.get() == nullptr) {
    return result;
  }
  WinHttpSetTimeouts(session.get(), 3000, 3000, 5000, 5000);
  InternetHandle connection(WinHttpConnect(
      session.get(), kReleaseApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (connection.get() == nullptr) {
    return result;
  }
  InternetHandle request(WinHttpOpenRequest(
      connection.get(),
      L"GET",
      kReleaseApiPath,
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      WINHTTP_FLAG_SECURE));
  if (request.get() == nullptr) {
    return result;
  }
  constexpr wchar_t headers[] =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28\r\n";
  if (WinHttpSendRequest(
          request.get(),
          headers,
          static_cast<DWORD>(std::size(headers) - 1),
          WINHTTP_NO_REQUEST_DATA,
          0,
          0,
          0) == FALSE ||
      WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return result;
  }
  DWORD status_code = 0;
  DWORD status_bytes = sizeof(status_code);
  if (WinHttpQueryHeaders(
          request.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &status_code,
          &status_bytes,
          WINHTTP_NO_HEADER_INDEX) == FALSE ||
      status_code != 200) {
    return result;
  }

  std::string response;
  char buffer[8192];
  for (;;) {
    DWORD bytes_read = 0;
    if (WinHttpReadData(
            request.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read) ==
        FALSE) {
      return result;
    }
    if (bytes_read == 0) {
      break;
    }
    if (response.size() + bytes_read > 2U * 1024U * 1024U) {
      return result;
    }
    response.append(buffer, bytes_read);
  }

  const std::string tag = latest_release_tag(response);
  const SemanticVersion latest = parse_semantic_version(tag);
  const SemanticVersion current =
      parse_semantic_version(SOUNDFORK_VERSION_STRING);
  if (!latest.valid || !current.valid) {
    return result;
  }
  result.latest_tag.assign(tag.begin(), tag.end());
  result.status =
      version_is_newer(latest, current) ? UpdateStatus::available
                                        : UpdateStatus::up_to_date;
  return result;
}

}  // namespace

UpdateResult check_for_updates() noexcept {
  try {
    return check_for_updates_impl();
  } catch (...) {
    return {};
  }
}

bool update_parser_self_test() noexcept {
  try {
    return version_is_newer(
               parse_semantic_version("v0.1.0-beta.1"),
               parse_semantic_version("0.1.0-alpha.4")) &&
           version_is_newer(
               parse_semantic_version("v0.1.0"),
               parse_semantic_version("0.1.0-rc.1")) &&
           latest_release_tag(
               R"([{"tag_name":"v0.1.0-alpha.4"},{"tag_name":"v0.1.0-beta.1"}])") ==
               "v0.1.0-beta.1";
  } catch (...) {
    return false;
  }
}

}  // namespace soundfork::control
