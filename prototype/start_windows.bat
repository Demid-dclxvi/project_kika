@echo off
chcp 65001 >nul
cd /d "%~dp0"
title Прочность печати

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (where python >nul 2>nul && set "PY=python")
if not defined PY goto nopython

if not exist ".venv\Scripts\python.exe" (
  echo Первый запуск: ставлю библиотеки, это займёт 1-3 минуты...
  %PY% -m venv .venv || goto nopython
  ".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
  ".venv\Scripts\python.exe" -m pip install -r requirements.txt || goto fail
)

".venv\Scripts\python.exe" -m fdmfea gui
pause
exit /b 0

:nopython
echo Не найден Python 3.10 или новее.
echo Установите его с https://www.python.org/downloads/ и при установке отметьте "Add python.exe to PATH".
pause
exit /b 1

:fail
echo Не удалось установить библиотеки. Проверьте интернет и запустите файл ещё раз.
rmdir /s /q .venv
pause
exit /b 1
