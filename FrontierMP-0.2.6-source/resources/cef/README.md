# FrontierMP CEF package

The build stages this directory at `cef/` next to `FrontierClient.dll`, matching the historical RDRMP package boundary.

The historical Chromium/CEF runtime is not vendored here. To overlay a complete runtime, configure:

cmake -DFRONTIER_CEF_PACKAGE_DIR="C:/path/to/client-rdrmp2/data/cef" ...

The historical main-menu URL is `cef/cef_ui/mainmenu/index.html`.
