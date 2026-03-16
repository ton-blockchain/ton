# Сборка и запуск симуляции

```
build/
├── simulation            ← основной бинарник (ConsensusHarness)
└── …
```

---

## Первичная настройка (один раз)

```bash
# Из корня tonGraph/
cmake -B build -DWITH_SIMULATION=ON
cmake --build build --target simulation -j$(nproc)
```

**Флаги**

| Флаг CMake | Назначение |
|---|---|
| `-DWITH_SIMULATION=ON` | Включает цель `ConsensusHarness` в `simulation/CMakeLists.txt` |
| `-DCMAKE_BUILD_TYPE=Debug` | Включает символы для lldb/gdb |

> `GRAPH_LOGGING_ENABLED` — переменная окружения (не CMake-флаг); управляет записью в файл во время выполнения.

---

## Известные проблемы сборки на Windows (VS 2026)

### 1. OpenSSL: неправильный Perl

**Симптом:**
```
Can't locate Locale/Maketext/Simple.pm in @INC
CMake Error at CMake/BuildOpenSSL.cmake:127: OpenSSL config failed with code 2
```

**Причина:** CMake подхватывает Git Bash Perl (`C:\Program Files\Git\usr\bin\perl.exe`), которому не хватает модулей CPAN.

**Решение:** запускать cmake через PowerShell/cmd, предварительно поставив Strawberry Perl (`C:\Strawberry\perl\bin`) **первым** в `PATH`:

```powershell
$env:PATH = 'C:\Strawberry\perl\bin;' + $env:PATH
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build -DWITH_SIMULATION=ON'
```

> Если `$(nproc)` не работает в PowerShell — замени на конкретное число, например `-j8`.

---

### 2. zlib: тулсет v142 (VS 2019) не найден

**Симптом:**
```
error MSB8020: Cannot find build tools for Visual Studio 2019 (PlatformToolset = "v142")
CMake Error at CMake/BuildZlib.cmake:51: Zlib build failed with code 1
```

**Причина:** `CMake/BuildZlib.cmake` жёстко прописывает `/p:PlatformToolset=v142`, которого нет в VS 2026.

**Решение:** в `CMake/BuildZlib.cmake` заменить оба вхождения `v142` на `v143` (VS 2022):
```cmake
# было:
COMMAND msbuild zlibstat.vcxproj ... /p:PlatformToolset=v142 ...
# стало:
COMMAND msbuild zlibstat.vcxproj ... /p:PlatformToolset=v143 ...
```

> `v143` (VS 2022, MSVC 14.4x) доступен при установленной VS 2022 Community.
> Изменение уже внесено в `CMake/BuildZlib.cmake`.

---

### 3. Полная команда configure (Windows, с учётом обоих фиксов)

```powershell
# PowerShell, из корня tonGraph/
$env:PATH = 'C:\Strawberry\perl\bin;' + $env:PATH
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build -DWITH_SIMULATION=ON'
```

Время configure: ~6 минут (OpenSSL + zlib собираются при первом запуске).

---

## Пересборка без полного cmake

```bash
# Только simulation-цель (быстро):
cmake --build build --target simulation -j4

# Только ConsensusHarness:
cmake --build build --target ConsensusHarness -j4
```

**Когда нужен полный cmake-reconfigure** (аналог рестарта dev-сервера):

```
изменения, требующие cmake -B build
├── simulation/CMakeLists.txt
├── CMakeLists.txt (корневой)
├── добавление новых .cpp файлов
└── изменение флагов компиляции
```

> Для правок внутри существующих `.cpp`/`.h` достаточно `cmake --build`.

---

## Запуск сценария

```bash
# Из корня tonGraph/
GRAPH_LOGGING_ENABLED=1 \
GRAPH_LOG_FILE=simulation/trace.ndjson \
./build/simulation/ConsensusHarness --scenario equivocation

# Доступные сценарии:
#   equivocation        — Byzantine валидатор голосует дважды
#   message_withholding — лидер удерживает propose
#   byzantine_leader    — лидер шлёт разные кандидаты
```

Бинарь записывает `simulation/trace.ndjson`.

---

## Запуск relay.mjs (NDJSON → Neo4j)

```bash
# Из корня tonGraph/
node simulation/relay.mjs
```

Читает `simulation/trace.ndjson` построчно и отправляет события через `AuraGraphReporter`.
Требует заполненного `.env` (см. [MCP_NEO4J_AURA.md](MCP_NEO4J_AURA.md)).

---

## Отладка сборки (Windows, PowerShell)

```powershell
# Найти ошибки компиляции
cmake --build build --target simulation 2>&1 | Select-String "error:"

# Проверить, что бинарь создан
Test-Path build/simulation/ConsensusHarness.exe
```

---

## Очистка трейс-файла перед новым прогоном

```bash
rm simulation/trace.ndjson
# или в PowerShell:
Remove-Item simulation/trace.ndjson -ErrorAction SilentlyContinue
```

> Не забудь также очистить граф в Neo4j: **[CYPHER_QUERIES.md#clean](CYPHER_QUERIES.md#clean)**.
