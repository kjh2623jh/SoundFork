#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

#include "diagnostics/async_log.hpp"

namespace {
using namespace soundfork::diagnostics;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void enqueue(AsyncLog& logger, Level level, std::string_view event, std::string_view detail = {}) {
  const auto before = logger.stats().accepted;
  for (int retry = 0; retry < 100; ++retry) {
    logger.write(level, event, detail);
    if (logger.stats().accepted != before) return;
    Sleep(1);
  }
  throw std::runtime_error("logger never accepted test record");
}
}
int main() {
  using namespace soundfork::diagnostics;
  std::filesystem::path root;
  try {
    root = std::filesystem::temp_directory_path() /
        (L"SoundForkLogTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(root), "private test directory exists");
    {
      AsyncLog logger;
      require(!logger.start({}, Component::control, "test"), "empty directory accepted");
      require(logger.start(root / L"basic", Component::control, "test"), "start failed");
      enqueue(logger, Level::info, "start", "line1\nline2\r\nend");
      logger.stop();
      const auto content = read(root / L"basic" / L"control.log");
      require(content.find("version=test event=start line1 line2  end\r\n") != std::string::npos,
          "missing record or newline injection");
      require(logger.stats().io_errors == 0 && logger.stats().written >= 1, "basic write failed");
    }
    {
      AsyncLog logger;
      require(logger.start(root / L"rotate", Component::relay, "test", 1024), "rotation start failed");
      for (int i = 0; i < 80; ++i) {
        logger.write(Level::info, "metrics", "queue_frames=256 dropped=0 underflow=0");
        Sleep(2);
      }
      logger.stop();
      require(logger.stats().io_errors == 0, "rotation IO error");
      std::size_t files = 0;
      for (const auto& file : std::filesystem::directory_iterator(root / L"rotate")) {
        ++files;
        require(file.file_size() <= 1024, "file exceeded cap");
      }
      require(files == 3, "expected current file and two backups");
      AsyncLog restarted;
      require(restarted.start(root / L"rotate", Component::relay, "restart", 1024), "restart failed");
      enqueue(restarted, Level::info, "restart");
      restarted.stop();
      require(read(root / L"rotate" / L"relay.log").find("event=restart") != std::string::npos,
          "restart did not append/rotate");
    }
    {
      AsyncLog logger;
      require(logger.start(root / L"load", Component::hub, "test", 1024), "load start failed");
      auto produce = [&] { for (int i = 0; i < 50000; ++i) logger.write(Level::info, "load"); };
      std::thread first(produce), second(produce);
      first.join(); second.join();
      logger.stop();
      const auto stats = logger.stats();
      require(stats.accepted + stats.dropped == 100000, "lost producer accounting");
      require(stats.dropped > 0 && stats.io_errors == 0, "backpressure not handled");
      for (const auto& file : std::filesystem::directory_iterator(root / L"load"))
        require(file.file_size() <= 1024, "load exceeded file cap");
    }
    {
      const auto blocked = root / L"not-a-directory";
      { std::ofstream fixture(blocked); fixture << "fixture"; }
      AsyncLog logger;
      require(logger.start(blocked, Component::control, "test"), "asynchronous start failed");
      enqueue(logger, Level::error, "write_denied");
      logger.stop();
      require(logger.stats().io_errors > 0 && read(blocked) == "fixture", "IO failure not isolated");
    }
    {
      const auto locked = root / L"locked";
      std::filesystem::create_directory(locked);
      const auto file = locked / L"control.log";
      const HANDLE blocker = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(blocker != INVALID_HANDLE_VALUE, "cannot create lock fixture");
      AsyncLog logger;
      require(logger.start(locked, Component::control, "test"), "locked-path start failed");
      enqueue(logger, Level::error, "locked_file");
      for (int retry = 0; retry < 100 && logger.stats().io_errors == 0; ++retry) Sleep(1);
      CloseHandle(blocker);
      require(logger.stats().io_errors > 0, "locked file did not report failure");
      Sleep(1100);  // Writer retry backoff; not an audio/production wait.
      enqueue(logger, Level::info, "recovered");
      logger.stop();
      require(read(file).find("event=recovered") != std::string::npos, "writer did not recover after unlock");
    }
    {
      const auto target = root / L"owned-target.txt";
      { std::ofstream fixture(target); fixture << "do not append"; }
      const auto linked = root / L"linked";
      std::filesystem::create_directory(linked);
      require(CreateHardLinkW((linked / L"control.log").c_str(), target.c_str(), nullptr) != FALSE,
          "cannot create owned hardlink fixture");
      AsyncLog logger;
      require(logger.start(linked, Component::control, "test"), "linked-path start failed");
      enqueue(logger, Level::error, "must_not_append");
      logger.stop();
      require(logger.stats().io_errors > 0 && read(target) == "do not append", "log followed hardlink");
    }
    std::filesystem::remove_all(root);
    std::cout << "Bounded async logs: rotation, restart, backpressure, injection and IO failure passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    // Keep this private test directory for failure inspection.
    return 1;
  }
}
