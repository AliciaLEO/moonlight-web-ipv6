@echo off
rem The flag window for Steam Remote Play: add this file as a non-Steam game on the host.
rem Kept in the foreground: Steam ends the stream when the "game" exits. Esc closes it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0flag-window.ps1" -Log "%~dp0flips.csv"
