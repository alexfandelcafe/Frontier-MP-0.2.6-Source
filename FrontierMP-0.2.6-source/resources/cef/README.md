# FrontierMP CEF package

The build stages this directory at cef/ next to FrontierClient.dll, matching the historical RDRMP package boundary.

The repository contains only FrontierMP's HTML frontend. Chromium/CEF binaries are not vendored. The build expects a CEF Windows distribution with the CEF SDK headers, libcef_dll, and Release/libcef.lib.

Configure with:

cmake --preset windows-vs2026-x64 -DFRONTIER_CEF_PACKAGE_DIR="C:/path/to/cef"

The historical main-menu URL is:

cef/cef_ui/mainmenu/index.html

When the package is configured, FrontierClient.dll starts a small CEF host thread using an external message pump, creates a child browser window over the RDR top-level window, and loads the URL above. FrontierCefSubprocess.exe handles CEF renderer/GPU subprocesses.

When FRONTIER_CEF_PACKAGE_DIR is empty, no CEF code is compiled and the client remains usable without the SDK.