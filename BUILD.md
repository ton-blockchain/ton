## Prerequisites for Windows
1. Install Visual Studio [2019](https://visualstudio.microsoft.com/downloads/).

2. Install [vcpkg](https://github.com/Microsoft/vcpkg) with the following console commands:
```
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
```
3. Add the System variable **VCPKG_DEFAULT_TRIPLET=x64-windows**.

Then run the following with administrator privileges:
```
vcpkg integrate install
```
If you encounter any issues, read [docs](https://github.com/microsoft/vcpkg/blob/master/docs/users/integration.md) about vcpkg integration.

4. Install all relevant dependencies:
```
vcpkg install openssl zlib getopt libmicrohttpd
```
5. Download [gperf](http://gnuwin32.sourceforge.net/packages/gperf.htm) and add it to your PATH

(Optional) Download [ninja-build](https://github.com/ninja-build/ninja/releases) and add it to your PATH

## Generate Visual Studio project
```
cd telegram-node
mkdir build
cd build
cmake -G "Visual Studio 16" -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]\\scripts\\buildsystems\\vcpkg.cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
```

## Fast build with ninja-build
Open "x64 Native Tools Command Prompt" for VS 2019
```
%comspec% /k "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
```
then:
```
cd telegram-node
mkdir build
cd build
cmake -G "Ninja" .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]\\scripts\\buildsystems\\vcpkg.cmake
ninja
```

## Building on Linux (Debian based)

Install dependencies:
```
sudo apt update && sudo apt upgrade 
sudo apt install cmake clang ninja-build pkg-config libssl-dev zlib1g-dev libreadline-dev gperf libmicrohttpd-dev
```

Make & build with a ninja-build (assuming the repository directory is titled "telegram-node"):
```
cd telegram-node
mkdir build
cd build
cmake -G "Ninja" ..
ninja
```

## Building on Mac

1. Install Xcode developer tools and [vcpkg](https://github.com/Microsoft/vcpkg)
2. Install all relevant dependencies:
```
vcpkg install openssl zlib getopt libmicrohttpd
```
3. Install ninja and cmake
```
brew install ninja
brew install cmake
```
4. Compile
```
cd telegram-node
mkdir build
cd build
cmake -G "Ninja" ..
ninja
```
