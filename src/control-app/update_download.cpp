#include "control-app/update_download.hpp"

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>

namespace soundfork::control {
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("Update download failed"); }
struct InternetCloser { void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); } };
using Internet = std::unique_ptr<void, InternetCloser>;

class Hash final {
 public:
  Hash() { require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
    if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
      throw std::runtime_error("SHA256 initialization failed");
    }
  }
  ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;
  void add(std::span<std::byte> bytes) {
    require(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0);
  }
  std::string finish() {
    std::array<UCHAR, 32> digest{};
    require(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

class HttpSource final : public UpdateSource {
 public:
  explicit HttpSource(const std::wstring& initial, DownloadProgress& progress) : progress_(progress) {
    session_.reset(WinHttpOpen(L"SoundFork updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(session_ != nullptr);
    require(WinHttpSetTimeouts(session_.get(), 3000, 3000, 5000, 5000) != FALSE);
    std::wstring url = initial;
    for (int redirects = 0; redirects <= 5; ++redirects) {
      require(!progress.cancel.load() && trusted_update_redirect(url));
      URL_COMPONENTS parts{sizeof(parts)};
      parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
      require(WinHttpCrackUrl(url.c_str(), 0, 0, &parts) != FALSE);
      if (!parts.lpszHostName || !parts.dwHostNameLength || !parts.lpszUrlPath || !parts.dwUrlPathLength)
        throw std::runtime_error("Invalid update URL");
      const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
      std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
      if (parts.dwExtraInfoLength) {
        if (!parts.lpszExtraInfo) throw std::runtime_error("Invalid update URL suffix");
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
      }
      connection_.reset(WinHttpConnect(session_.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
      require(connection_ != nullptr);
      request_.reset(WinHttpOpenRequest(connection_.get(), L"GET", path.c_str(), nullptr,
          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
      require(request_ != nullptr);
      DWORD disable = WINHTTP_DISABLE_REDIRECTS;
      require(WinHttpSetOption(request_.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) != FALSE);
      require(WinHttpSendRequest(request_.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE);
      require(WinHttpReceiveResponse(request_.get(), nullptr) != FALSE);
      DWORD status = 0, bytes = sizeof(status);
      require(WinHttpQueryHeaders(request_.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) != FALSE);
      if (status == 200) return;
      require(status == 301 || status == 302 || status == 303 || status == 307 || status == 308);
      bytes = 0;
      WinHttpQueryHeaders(request_.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
          nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
      require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes <= 32768 && bytes >= sizeof(wchar_t));
      std::vector<wchar_t> location(bytes / sizeof(wchar_t) + 1, L'\0');
      require(WinHttpQueryHeaders(request_.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
          location.data(), &bytes, WINHTTP_NO_HEADER_INDEX) != FALSE);
      url.assign(location.data());
      request_.reset(); connection_.reset();
    }
    throw std::runtime_error("Too many redirects");
  }
  std::size_t read(std::span<std::byte> buffer) override {
    require(!progress_.cancel.load());
    DWORD count = 0;
    require(WinHttpReadData(request_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count) != FALSE);
    return count;
  }
 private:
  DownloadProgress& progress_;
  Internet session_, connection_, request_;
};

std::filesystem::path update_directory() {
  PWSTR local = nullptr;
  require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)));
  struct Free { PWSTR value; ~Free() { CoTaskMemFree(value); } } free{local};
  GUID id{};
  require(SUCCEEDED(CoCreateGuid(&id)));
  wchar_t name[40]{};
  require(StringFromGUID2(id, name, static_cast<int>(std::size(name))) != 0);
  return std::filesystem::path(local) / L"SoundFork" / L"Updates" / name;
}
}  // namespace

bool trusted_update_redirect(std::wstring_view url) noexcept {
  // Require an absolute HTTPS URL, no credentials/non-default port, and exact
  // GitHub download hosts. Redirects are checked manually at every hop.
  URL_COMPONENTS parts{sizeof(parts)};
  parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
  if (url.size() > 16384 || !WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
      parts.dwUserNameLength || parts.dwPasswordLength || !parts.lpszHostName || !parts.dwHostNameLength) return false;
  const std::wstring_view host(parts.lpszHostName, parts.dwHostNameLength);
  return host == L"github.com" || host == L"release-assets.githubusercontent.com" ||
      host == L"objects.githubusercontent.com";
}

DownloadedUpdate::~DownloadedUpdate() { reset(); }
void DownloadedUpdate::reset() noexcept {
  file.reset();
  try {
    if (!retained_ && !path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.parent_path(), ignored);
    }
  } catch (...) { /* Cleanup must not terminate the control application. */ }
  path.clear(); retained_ = false; status = DownloadStatus::failed;
}

bool verify_download(const UpdateResult& release, const DownloadedUpdate& downloaded) noexcept {
  try {
    if (!downloadable_update(release) || !downloaded.file) return false;
    LARGE_INTEGER zero{}, size{};
    if (!GetFileSizeEx(downloaded.file.value, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) != release.installer_size ||
        !SetFilePointerEx(downloaded.file.value, zero, nullptr, FILE_BEGIN)) return false;
    Hash hash;
    std::array<std::byte, 8192> buffer{};
    for (;;) {
      DWORD read = 0;
      if (!ReadFile(downloaded.file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
      if (!read) break;
      hash.add(std::span(buffer).first(read));
    }
    return hash.finish() == release.sha256;
  } catch (...) { return false; }
}

void download_update_from_source(const UpdateResult& release, UpdateSource& source,
    DownloadProgress& progress, const std::filesystem::path& directory, DownloadedUpdate& output) noexcept {
  output.reset(); progress.received = 0;
  bool directory_created = false;
  try {
    require(downloadable_update(release));
    if (progress.cancel.load()) { output.status = DownloadStatus::cancelled; return; }
    // Never reuse a directory/file or overwrite an earlier downloaded installer.
    std::filesystem::create_directories(directory.parent_path());
    require(std::filesystem::create_directory(directory));
    directory_created = true;
    const auto path = directory / release.installer_name;
    output.file.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(static_cast<bool>(output.file));
    output.path = path;
    std::array<std::byte, 8192> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    std::uint64_t total = 0;
    while (!progress.cancel.load()) {
      require(std::chrono::steady_clock::now() < deadline);
      const auto read = source.read(buffer);
      if (!read) break;
      require(read <= buffer.size() && read <= release.installer_size - total);
      DWORD written = 0;
      require(WriteFile(output.file.value, buffer.data(), static_cast<DWORD>(read), &written, nullptr) != FALSE && written == read);
      total += read; progress.received = total;
    }
    if (progress.cancel.load()) { output.reset(); output.status = DownloadStatus::cancelled; return; }
    require(total == release.installer_size);
    // DuplicateHandle would retain the writable FILE_OBJECT and can prevent
    // Windows from mapping an executable image even with a read-only duplicate.
    // ReOpenFile creates distinct read-only file objects for the same file.
    // Retain deny-delete sharing throughout; verify after obtaining deny-write.
    platform::UniqueHandle readonly;
    readonly.reset(ReOpenFile(output.file.value, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_SEQUENTIAL_SCAN));
    require(static_cast<bool>(readonly));
    output.file = std::move(readonly);
    readonly.reset(ReOpenFile(output.file.value, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_SEQUENTIAL_SCAN));
    require(static_cast<bool>(readonly));
    output.file = std::move(readonly);
    require(verify_download(release, output));
    output.status = DownloadStatus::ready;
  } catch (...) {
    output.reset();
    if (directory_created) {
      try { std::error_code ignored; std::filesystem::remove(directory, ignored); } catch (...) {}
    }
    if (progress.cancel.load()) output.status = DownloadStatus::cancelled;
  }
}

void download_update(const UpdateResult& release, DownloadProgress& progress, DownloadedUpdate& output) noexcept {
  try {
    if (!downloadable_update(release)) { output.reset(); return; }
    HttpSource source(release.installer_url, progress);
    download_update_from_source(release, source, progress, update_directory(), output);
  } catch (...) {
    output.reset();
    if (progress.cancel.load()) output.status = DownloadStatus::cancelled;
  }
}
}  // namespace soundfork::control
