#!/bin/sh
pushd .
mkdir -p build_native
cd build_native
cmake -DTON_ONLY_TONLIB=ON .. || exit 1
cmake --build . --target prepare_cross_compiling || exit 2
#cmake --build . --target tl_generate_java || exit 1
popd
php AddIntDef.php src/drinkless/org/ton/TonApi.java || exit 1

./build-all.sh || exit 1

rm -rf tonlib_export
mkdir -p tonlib_export/tonlib
echo src libs | xargs tar -c | tar -C tonlib_export/tonlib -xv

pushd .
cd tonlib_export/tonlib
#TODO javadoc
#javadoc -d javadoc -bootclasspath $ANDROID_SDK_ROOT/platforms/android-28/android.jar -extdirs ../../../../annotations/ -classpath java org.drinkless.td.libcore.telegram
popd

cd tonlib_export
find . -name '.DS_Store' -type f -print0 | xargs -0 rm -f
jar -cMf tonlib_debug.zip tonlib
#zip -r tonlib_debug libtd
rm tonlib/libs/*/*.debug
jar -cMf tonlib.zip tonlib
#zip -r tonlib libtd
