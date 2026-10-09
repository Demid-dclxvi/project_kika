#!/usr/bin/env bash
# Запуск приложения «Прочность печати» на macOS и Linux
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3}
if ! command -v "$PY" >/dev/null 2>&1; then
  echo "Не найден python3. Установите Python 3.10+ (macOS: https://www.python.org/downloads/, Linux: пакет python3 и python3-venv)."
  exit 1
fi
if [ ! -x .venv/bin/python ]; then
  echo "Первый запуск: ставлю библиотеки, это займёт 1-3 минуты..."
  "$PY" -m venv .venv
  .venv/bin/python -m pip install --upgrade pip >/dev/null
  .venv/bin/python -m pip install -r requirements.txt || { rm -rf .venv; echo "Не удалось установить библиотеки. Проверьте интернет и запустите ещё раз."; exit 1; }
fi
exec .venv/bin/python -m fdmfea gui "$@"
