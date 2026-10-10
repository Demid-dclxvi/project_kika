@echo off
rem Kika: start the window with software OpenGL (no video driver, remote desktop, VM).
set QT_OPENGL=software
start "" "%~dp0kika-gui.exe" %*
