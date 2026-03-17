# Первичная настройка (один раз)

Выполняется один раз при первом клоне репозитория.

---

## Configure и сборка

```bash
# Из корня tonGraph/
cmake -B build -DWITH_SIMULATION=ON
cmake --build build --target simulation -j$(nproc)
```

**Флаги CMake**

| Флаг | Назначение |
|---|---|
| `-DWITH_SIMULATION=ON` | Включает цель `ConsensusHarness` в `simulation/CMakeLists.txt` |
| `-DCMAKE_BUILD_TYPE=Debug` | Включает символы для lldb/gdb |

> `GRAPH_LOGGING_ENABLED` — переменная окружения (не CMake-флаг); управляет записью в файл во время выполнения.

---

## Известные проблемы сборки на Windows (VS 2022)

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
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build -DWITH_SIMULATION=ON'
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
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build -DWITH_SIMULATION=ON'
```

Время configure: ~6 минут (OpenSSL + zlib собираются при первом запуске).
