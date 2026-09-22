# Frontier build probe

The build probe is a one-shot console diagnostic. It exits after printing the result.

Run it from a terminal, or use `scripts_run_build_probe.bat` from the project root.

```powershell
.\build\bin\Release\frontier_build_probe.exe "C:\Path\To\Red Dead Redemption\RDR.exe"
```

Save the complete console output. Do not upload the executable itself.

The output is intended to become the verified build record in `game/src/build_fingerprint.cpp` after independent symbol checks.
