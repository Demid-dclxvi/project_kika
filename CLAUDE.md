# Заметки для Claude

Проект: расчёт прочности деталей для FDM-печати по G-code; ядро переносится с прототипа на Python (`prototype/`) на C++20. Автор — Демид. Проектный документ (этапы, сроки, решения): https://claude.ai/code/artifact/f01a13ed-8ef5-4a14-8678-5e1580c6a166 — вкладка «Техническая часть» и вложенная «Вдвоём: прототип и MVP».

## Как работаем

- Общение, документы, сообщения коммитов и сообщения пользователю — по-русски. Идентификаторы — по-английски, комментарии — по-русски.
- `prototype/` — эталон, не изменять. Каждый перенесённый модуль сверяется с ним (`tools/compare_with_prototype.py`) и покрывается тестами в `tests/`.
- Сознательные отличия от прототипа записывать в `docs/ARCHITECTURE.md`, таблица «Отличия от прототипа». Состояние модулей — там же и в README.
- Новые зависимости — только через `vcpkg.json` и только с допустимой лицензией; сразу вносить в `THIRD_PARTY.md`. AGPL не использовать.
- Единицы мм, Н, МПа; нотация Фойгта `[11, 22, 33, 23, 13, 12]`, инженерные сдвиги.

## Сборка и проверка

- Пресеты: `win-debug`, `win-release`, `linux-debug`, `linux-release`; в CI — `ci-windows`, `ci-linux` (предупреждения = ошибки).
- `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release`.
- Ядро — без сторонних библиотек (JSON свой, `kika::json`). Отчёт (`src/report`) использует zlib; через vcpkg подключаются zlib и Catch2 (тесты). Новые зависимости в ядро — только если своё писать неразумно.
- Файлы просмотрщика отчёта (`src/report/web`) встраиваются в программу скриптом `cmake/EmbedFiles.cmake`. Отчёт проверять в браузере: Chromium есть в среде (Playwright для Node — `NODE_PATH=/opt/npm-tools/node_modules`), WebGL — через `--use-angle=swiftshader --enable-unsafe-swiftshader`.
- В облачной среде Claude скачивание исходников библиотек с github.com/codeload закрыто сетевой политикой, поэтому vcpkg там не собирает Catch2. Ядро, `src/report` и `apps/cli` проверять прямой сборкой g++ и clang++ (`-std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Werror`, нужны `-pthread` и системная zlib `-lz`), полную сборку с тестами — через GitHub Actions (`gh run watch`; логи CI оттуда не скачиваются, видны только статусы шагов).
- Сверка с прототипом: `python tools/compare_with_prototype.py --kika <путь к kika>` (нужны numpy и scipy).
- Демид собирает на Windows в Visual Studio; готовый `kika.exe` берёт из артефактов CI.
