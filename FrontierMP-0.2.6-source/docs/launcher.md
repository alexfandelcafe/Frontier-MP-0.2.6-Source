# Launcher

The launcher starts the user's existing `RDR.exe` without modifying files in the game installation.

It launches the process suspended, loads `FrontierClient.dll` into the game process, supplies FrontierMP connection settings through process environment variables, and then resumes RDR1.

The installed game remains the user's own dependency. FrontierMP does not contain or replace the game executable or game assets.
