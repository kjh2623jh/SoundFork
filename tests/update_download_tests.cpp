#include "control-app/update_checker.hpp"
#include "control-app/update_download.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#include <stdexcept>

using namespace soundfork::control;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
const char* kRelease = R"({"tag_name":"v99.0.0","draft":false,"prerelease":false,
"body":"## Changes\n\ud55c\uae00 \ud83c\udfb5 \"tag_name\":\"v999.0.0\"",
"assets":[{"name":"SoundFork-Setup-v99.0.0.exe","size":3,
"digest":"sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
"browser_download_url":"https://github.com/kjh2623jh/SoundFork/releases/download/v99.0.0/SoundFork-Setup-v99.0.0.exe"}]})";
class Source final : public UpdateSource {
 public:
  std::string data = "abc";
  std::size_t offset = 0;
  bool fail = false;
  DownloadProgress* cancel = nullptr;
  std::size_t read(std::span<std::byte> out) override {
    if (fail) throw std::runtime_error("network failure");
    if (cancel) cancel->cancel = true;
    if (offset == data.size()) return 0;
    // Chunk boundaries must not change hashing or progress.
    out[0] = static_cast<std::byte>(data[offset++]);
    return 1;
  }
};

// Independent CryptoAPI hash for a real PE fixture (this test executable).
// SEC_IMAGE maps it as data and never runs it or elevates anything.
std::string fixture_hash(const std::string& bytes) {
  HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
  struct Cleanup { HCRYPTPROV& provider; HCRYPTHASH& hash;
    ~Cleanup() { if (hash) CryptDestroyHash(hash); if (provider) CryptReleaseContext(provider, 0); }
  } cleanup{provider, hash};
  require(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE &&
      CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE &&
      CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) != FALSE, "fixture hash setup");
  BYTE digest[32]{}; DWORD size = sizeof(digest);
  require(CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != FALSE, "fixture hash result");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
}

int main() try {
  require(update_parser_self_test(), "version regression");
  auto release = parse_update_release(kRelease);
  require(release.status == UpdateStatus::available && downloadable_update(release), "valid release rejected");
  require(release.latest_tag == L"v99.0.0" && release.release_notes.find(L"\ud55c\uae00 \U0001f3b5") != std::wstring::npos,
      "Unicode notes/embedded tag decoded incorrectly");
  for (const auto json : {"{}", "[]", "not json", R"({"tag_name":"v99.0.0","draft":true,"prerelease":false})",
      R"({"tag_name":"v99.0.0","draft":false,"prerelease":true})",
      R"({"tag_name":"v99.0.0-beta.1","draft":false,"prerelease":false})"})
    require(parse_update_release(json).status == UpdateStatus::failed, "invalid release accepted");
  for (const auto body : {R"("body":null)", R"("body":"")"}) {
    const auto parsed = parse_update_release(std::string(R"({"tag_name":"v99.0.0","draft":false,"prerelease":false,)") + body + "}");
    require(parsed.status == UpdateStatus::available && parsed.release_notes.empty() && !downloadable_update(parsed), "missing notes/asset fallback");
  }
  for (const auto from : {std::string("sha256:"), std::string("SoundFork-Setup-v99.0.0.exe"), std::string("https://github.com/")}) {
    std::string damaged(kRelease); damaged.replace(damaged.find(from), from.size(), "invalid");
    require(!downloadable_update(parse_update_release(damaged)), "unverifiable/wrong asset accepted");
  }
  for (const auto url : {L"http://github.com/file", L"https://github.com.evil.test/x", L"https://github.com:444/x",
      L"https://user@github.com/x", L"https://evil.test/", L"/relative", L"file:///C:/test.exe"})
    require(!trusted_update_redirect(url), "unsafe redirect accepted");
  require(trusted_update_redirect(L"https://release-assets.githubusercontent.com/example?sig=test"), "GitHub redirect rejected");
  const auto root = std::filesystem::temp_directory_path() / (L"SoundForkUpdateTest-" + std::to_wstring(GetCurrentProcessId()));
  require(std::filesystem::create_directory(root), "test root already exists");
  struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup{root};
  for (int mode = 0; mode != 8; ++mode) {
    Source source; DownloadProgress progress; DownloadedUpdate output;
    auto metadata = release;
    if (mode == 1) source.data = "abd";
    if (mode == 2) source.data = "ab";
    if (mode == 3) source.data = "abcd";
    if (mode == 4) source.fail = true;
    if (mode == 5) source.cancel = &progress;
    if (mode == 6) metadata.installer_size = 0;
    if (mode == 7) progress.cancel = true;
    const auto directory = root / std::to_wstring(mode);
    download_update_from_source(metadata, source, progress, directory, output);
    if (mode == 0) {
      require(output.status == DownloadStatus::ready && progress.received == 3 && verify_download(metadata, output), "valid download failed");
      const auto file = output.path;
      soundfork::platform::UniqueHandle overwrite(CreateFileW(file.c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
      require(!overwrite && !DeleteFileW(file.c_str()), "verified installer can be modified/deleted while held");
      auto changed = metadata; changed.sha256[0] = '0';
      require(!verify_download(changed, output), "modified metadata accepted");
      output.reset();
      require(!std::filesystem::exists(directory), "successful download cleanup failed");
    } else {
      require(output.status == ((mode == 5 || mode == 7) ? DownloadStatus::cancelled : DownloadStatus::failed), "invalid/cancelled download succeeded");
      require(output.path.empty() && !output.file, "failed file retained");
      if (mode == 7) require(source.offset == 0 && !std::filesystem::exists(directory), "cancelled download performed I/O");
    }
  }
  // Existing directories/files must not be overwritten or cleaned up by retry.
  Source source; DownloadProgress progress; DownloadedUpdate output;
  const auto existing = root / L"existing";
  std::filesystem::create_directory(existing);
  const auto marker = existing / release.installer_name;
  soundfork::platform::UniqueHandle marker_file(CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, 0, nullptr)); marker_file.reset();
  download_update_from_source(release, source, progress, existing, output);
  require(output.status == DownloadStatus::failed && std::filesystem::exists(marker), "existing file overwritten/deleted");
  {
    std::vector<wchar_t> executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length != 0 && length < executable.size(), "PE fixture path");
    std::ifstream file(executable.data(), std::ios::binary);
    Source pe;
    pe.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    require(!pe.data.empty(), "PE fixture empty");
    auto metadata = release; metadata.installer_size = pe.data.size(); metadata.sha256 = fixture_hash(pe.data);
    // Batch this larger fixture; tiny split chunks were covered above.
    class PeSource final : public UpdateSource {
     public:
      explicit PeSource(const std::string& bytes) : data(bytes) {}
      std::size_t read(std::span<std::byte> buffer) override {
        const auto count = (std::min)(buffer.size(), data.size() - offset);
        if (count) std::memcpy(buffer.data(), data.data() + offset, count);
        offset += count; return count;
      }
     private: const std::string& data; std::size_t offset = 0;
    } pe_source(pe.data);
    DownloadedUpdate image; DownloadProgress image_progress;
    download_update_from_source(metadata, pe_source, image_progress, root / L"image", image);
    require(image.status == DownloadStatus::ready, "real PE download verification failed");
    soundfork::platform::UniqueHandle mapping(CreateFileMappingW(image.file.value, nullptr,
        PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr));
    require(static_cast<bool>(mapping), "verified installer file cannot be mapped by Windows as an image");
  }
  std::cout << "Release metadata, Unicode notes, HTTPS policy, hashing, cancellation, cleanup and file locking passed.\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
