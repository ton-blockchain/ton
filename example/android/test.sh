#!/bin/sh
cp -r src/* ./test/ton/src/main/java/
mkdir -p ./test/ton/src/cpp/prebuilt/
cp -r libs/* ./test/ton/src/cpp/prebuilt/
cd test
./gradlew connectedAndroidTest
