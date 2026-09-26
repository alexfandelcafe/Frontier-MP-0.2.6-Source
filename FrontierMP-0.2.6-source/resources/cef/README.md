# FrontierMP CEF package

The build stages this directory at cef/ next to FrontierClient.dll, matching the historical RDRMP package boundary.

The repository contains only FrontierMP's HTML frontend. Chromium/CEF binaries are not vendored. The build expects a CEF Windows distribution with the CEF SDK headers, libcef_dll, and Release/libcef.lib.

Configure with:

cmake --preset windows-vs2026-x64 -DFRONTIER_CEF_PACKAGE_DIR="C:/path/to/cef"

The historical main-menu URL is:

cef/cef_ui/mainmenu/index.html

When the package is configured, FrontierClient.dll initializes CEF on the RDR game thread, creates a windowless/off-screen browser and loads the URL above. FrontierCefSubprocess.exe owns renderer-process startup and installs the window.app bridge.

CEF is not responsible for rendering its own opaque window. The page is painted off-screen and the latest BGRA frame is composited into the RDR D3D11 swapchain so the native RDR 3D scene remains visible outside the HTML controls.

The renderer bridge currently exposes:
- app.connect(host, port)
- app.quit()

When FRONTIER_CEF_PACKAGE_DIR is empty, no CEF code is compiled and the client remains usable without the SDK.