# Probe: проверка сборки simulation в GitHub Codespaces

## Цель
Убедиться, что `simulation/ConsensusHarness` собирается и запускается в Codespaces (Ubuntu 24.04).

---

## Шаги

### 1. Установить зависимости

```bash
sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build libjemalloc-dev ccache nodejs

# Clang-21 (нужен для сборки TON)
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" \
  | sudo tee /etc/apt/sources.list.d/llvm.list
sudo apt update && sudo apt install -y clang-21
```

### 2. Сабмодули

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

### 3. Конфигурация CMake

```bash
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang-21 \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DTON_ARCH=x86-64 \
  -GNinja
```

> Если флаг `-DWITH_SIMULATION=ON` ещё не реализован — пока пропускаем;
> цель сейчас — убедиться, что validator-session и consensus компилируются.

### 4. Пробная сборка (validator-session — без simulation)

```bash
ninja validator-session -j4 2>&1 | tail -30
```

Ожидаемый результат: `[N/N] Linking CXX ...` без ошибок.

### 5. Проверить, что consensus-файлы компилируются

```bash
ninja validator -j4 2>&1 | tail -30
```

### 6. Проверить наличие Node.js для relay.mjs

```bash
node --version   # нужен >=18
npm --version
```

### 7. Проверить .env

```bash
cp .env.example .env   # если есть
# Убедиться, что NEO4J_URI / NEO4J_USER / NEO4J_PASSWORD заполнены
cat .env
```

---

## Ожидаемый результат

| Проверка | Ожидание |
|---|---|
| `ninja validator-session` | exit 0, нет C++ ошибок |
| `ninja validator` | exit 0 |
| `node --version` | v18+ |
| `cat .env` | `NEO4J_URI` непустой |

---

## Что проверить агенту

1. Скопировать вывод каждого шага.
2. Если `ninja` падает — сохранить первые 50 строк ошибки (`2>&1 | head -50`).
3. Если нет `clang-21` — попробовать `clang-20` или `clang` из пакетов по умолчанию.
4. Если нет `libjemalloc-dev` — убрать флаг `-DTON_USE_JEMALLOC=ON` из cmake.
5. Итог: написать "OK" / "FAILED: <шаг>" для каждой строки таблицы выше.
