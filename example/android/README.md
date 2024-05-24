# Generation of Tonlib libraries for Android OS
**Tl;dr** Download the latest version of Tonlib libraries for Android from TON release page or check the artifacts from [Android JNI GitHub action](https://github.com/ton-blockchain/ton/actions/workflows/tonlib-android-jni.yml).

## Compile Tonlib for Android manually 
Prerequisite: installed Java and set environment variable JAVA_HOME. 
```bash
git clone --recursive https://github.com/ton-blockchain/ton.git
cd ton
cp assembly/android/build-android-tonlib.sh .
chmod +x build-android-tonlib.sh
sudo -E ./build-android-tonlib.sh
```
# Generation of Tonlib libraries for iOS in Xcode

1. Clone repository https://github.com/labraburn/tonlib-xcframework
2. Open repository directory in Terminal
3. Run command:
```bash
swift run builder --output ./build --clean
```
5. Run command:
```bash
echo ./build/TON.xcframework/* | xargs -n 1 cp -R ./Resources/Headers
````
7. Import **OpenSSL.xcframework** and **TON.xcframework** in XCode in section _"Frameworks, Libraries, and Embedded Content"_
8. Now you can start using Tonlib client by importing it in C or Objective-C source files:
```objective-c
#import <tonlib/tonlib_client_json.h>
```

# Generation of Tonlib libraries for Desktop applications
You can use Tonlib compiled in an ordinary way for desktop applications. If you use Java you can load the library using JNA.

The latest Tonlib library can be found among other TON artifacts either on TON release page or inside the [appropriate GitHub action](https://github.com/ton-blockchain/ton/actions/).