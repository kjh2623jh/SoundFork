#pragma once

// Shared by the standalone relay executable and the optional in-process
// application-share relay. This function is called only from worker threads;
// it is never called by the ASIO real-time callback.
int soundfork_relay_main(int argc, wchar_t** argv);

