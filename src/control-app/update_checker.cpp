#include "control-app/update_checker.hpp"

#include <windows.h>
#include <winhttp.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cstdint>
#include <cmath>
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
    L"/repos/kjh2623jh/SoundFork/releases/latest";
// GitHub's latest-release endpoint excludes drafts and prereleases, including
// when recent development releases would otherwise fill an entire list page.
constexpr int kStableStage = 4;

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
    version.stage = kStableStage;
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
  DWORD disable = WINHTTP_DISABLE_REDIRECTS;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) return result;
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

  return parse_update_release(response);
}

}  // namespace

UpdateResult check_for_updates() noexcept {
  try {
    return check_for_updates_impl();
  } catch (...) {
    return {};
  }
}

bool downloadable_update(const UpdateResult& release) noexcept try {
  for (const auto character : release.latest_tag) if (character > 127) return false;
  const auto version = parse_semantic_version(winrt::to_string(release.latest_tag));
  if (!version.valid || version.stage != kStableStage ||
      release.latest_tag.empty() || release.latest_tag.front() != L'v' ||
      release.installer_size == 0 || release.installer_size > 128U * 1024U * 1024U ||
      release.sha256.size() != 64) return false;
  for (const auto digit : release.sha256) {
    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
  }
  return release.installer_name == L"SoundFork-Setup-" + release.latest_tag + L".exe" &&
      release.installer_url == L"https://github.com/kjh2623jh/SoundFork/releases/download/" +
          release.latest_tag + L"/" + release.installer_name;
} catch (...) { return false; }

UpdateResult parse_update_release(std::string_view json) noexcept {
  // Use the OS JSON parser, including Unicode/surrogate handling, rather than
  // searching strings (release notes may themselves contain JSON-looking text).
  const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
  struct Apartment {
    HRESULT hr;
    ~Apartment() { if (SUCCEEDED(hr)) RoUninitialize(); }
  } apartment{initialized};
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
  try {
    if (json.size() > 2U * 1024U * 1024U) return {};
    using namespace winrt::Windows::Data::Json;
    // Avoid a process-wide cached activation factory outliving this worker's
    // apartment. Tests also exercise repeated calls without an outer COM scope.
    const auto factory = winrt::get_activation_factory<IJsonObjectStatics>(L"Windows.Data.Json.JsonObject");
    const auto root = factory.Parse(winrt::to_hstring(json));
    if (root.GetNamedBoolean(L"draft", true) || root.GetNamedBoolean(L"prerelease", true)) return {};
    const std::wstring tag(root.GetNamedString(L"tag_name"));
    const auto version = parse_semantic_version(winrt::to_string(tag));
    const auto current = parse_semantic_version(SOUNDFORK_VERSION_STRING);
    if (!version.valid || version.stage != kStableStage || !current.valid) return {};
    UpdateResult result;
    result.latest_tag = tag;
    result.status = version_is_newer(version, current) ? UpdateStatus::available : UpdateStatus::up_to_date;
    const auto notes = root.TryLookup(L"body");
    if (notes && notes.ValueType() == JsonValueType::String) result.release_notes = notes.GetString();
    const auto assets_value = root.TryLookup(L"assets");
    if (!assets_value || assets_value.ValueType() != JsonValueType::Array) return result;
    const auto assets = assets_value.GetArray();
    for (const auto& value : assets) {
      if (value.ValueType() != JsonValueType::Object) continue;
      const auto asset = value.GetObject();
      const std::wstring expected = L"SoundFork-Setup-" + tag + L".exe";
      if (asset.GetNamedString(L"name", L"") != expected) continue;
      const auto digest_value = asset.TryLookup(L"digest");
      if (!digest_value || digest_value.ValueType() != JsonValueType::String) continue;
      const auto digest = winrt::to_string(digest_value.GetString());
      const double size = asset.GetNamedNumber(L"size", 0);
      if (!std::isfinite(size) || size < 1 || size > 128U * 1024U * 1024U || digest.substr(0, 7) != "sha256:") continue;
      UpdateResult candidate = result;
      candidate.installer_name = expected;
      candidate.installer_url = asset.GetNamedString(L"browser_download_url", L"");
      candidate.installer_size = static_cast<std::uint64_t>(size);
      if (static_cast<double>(candidate.installer_size) != size) continue;
      candidate.sha256 = digest.substr(7);
      if (downloadable_update(candidate)) return candidate;
    }
    return result;
  } catch (...) { return {}; }
}

bool update_parser_self_test() noexcept {
  try {
    constexpr std::string_view excluded[] = {
        "v9.0.0-dev", "v9.0.0-alpha.1", "v9.0.0-beta.1", "v9.0.0-rc.1",
        "v9.0.0-preview.1", "not-a-version", "v1.2.3/other",
        "v18446744073709551616.0.0"};
    for (const auto tag : excluded) {
      const auto json = std::string("{\"tag_name\":\"") + std::string(tag) + "\",\"draft\":false,\"prerelease\":false}";
      if (parse_update_release(json).status != UpdateStatus::failed) return false;
    }
    return version_is_newer(
               parse_semantic_version("v0.1.0-beta.1"),
               parse_semantic_version("0.1.0-alpha.4")) &&
           version_is_newer(
               parse_semantic_version("v0.1.0"),
               parse_semantic_version("0.1.0-rc.1")) &&
           parse_update_release(R"({"tag_name":"v1.2.2","draft":false,"prerelease":false})").latest_tag == L"v1.2.2" &&
           !version_is_newer(parse_semantic_version("v1.2.1"), parse_semantic_version("1.2.2")) &&
           !version_is_newer(parse_semantic_version("v1.2.2"), parse_semantic_version("1.2.2")) &&
           version_is_newer(parse_semantic_version("v1.2.3"), parse_semantic_version("1.2.2")) &&
           version_is_newer(parse_semantic_version("v1.3.0"), parse_semantic_version("1.3.0-beta.1"));
  } catch (...) {
    return false;
  }
}

}  // namespace soundfork::control
