# Tonlib C++ basic usage examples

Tonlib should be prebuilt and installed to local subdirectory `tonlib/`:
```
cd <path to Ton sources>
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=../example/cpp/tonlib ..
cmake --build . --target install
```

Then you can build the examples:
```
cd <path to Ton sources>/example/cpp
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DTonlib_DIR=<full path to Ton sources>/example/cpp/tonlib/lib/cmake/Tonlib ..
cmake --build .
```

To run `tonjson_example` you may need to manually copy a `tonlibjson` shared library from `tonlib/bin` to a directory containing built binaries.
