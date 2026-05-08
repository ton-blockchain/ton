#!/bin/bash

if [ ! -d "artifacts" ]; then
  echo "No artifacts found."
  exit 2
fi
# x86_64 or aarch64
ARCH=$1

rm -rf appimages

mkdir -p appimages/artifacts

wget -nc https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage
chmod +x ./appimagetool-$ARCH.AppImage

find_system_lib() {
  local lib_name=$1
  ldconfig -p | awk -v n="$lib_name" '$1 == n {print $NF; exit}'
}

copy_system_lib() {
  local lib_name=$1
  local dst_dir=$2
  local lib_path
  lib_path=$(find_system_lib "$lib_name")
  if [ -z "$lib_path" ]; then
    echo "Required library not found: $lib_name"
    exit 1
  fi
  cp "$lib_path" "$dst_dir/"
}

cd appimages
for file in ../artifacts/*; do
  if [[ -f "$file" && "$file" != *.so ]]; then
    appName=$(basename "$file")
    echo $appName
    # prepare AppDir
    mkdir -p $appName.AppDir/usr/{bin,lib}
    cp ../AppRun $appName.AppDir/AppRun
    sed -i "s/app/$appName/g" $appName.AppDir/AppRun
    chmod +x ./$appName.AppDir/AppRun
    printf '[Desktop Entry]\nName='$appName'\nExec='$appName'\nIcon='$appName'\nType=Application\nCategories=Utility;\n' > $appName.AppDir/$appName.desktop
    cp ../ton.png $appName.AppDir/$appName.png
    cp $file $appName.AppDir/usr/bin/

    copy_system_lib libatomic.so.1 $appName.AppDir/usr/lib
    copy_system_lib libreadline.so.8 $appName.AppDir/usr/lib
    copy_system_lib libgsl.so.27 $appName.AppDir/usr/lib
    copy_system_lib libblas.so.3 $appName.AppDir/usr/lib
    copy_system_lib libgslcblas.so.0 $appName.AppDir/usr/lib

    # New Linux builds use clang + libc++; keep libstdc++ fallback for older artifacts.
    if find_system_lib libc++.so.1 >/dev/null; then
      copy_system_lib libc++.so.1 $appName.AppDir/usr/lib
      copy_system_lib libc++abi.so.1 $appName.AppDir/usr/lib
      if find_system_lib libunwind.so.1 >/dev/null; then
        copy_system_lib libunwind.so.1 $appName.AppDir/usr/lib
      fi
    else
      copy_system_lib libstdc++.so.6 $appName.AppDir/usr/lib
    fi

    chmod +x ./$appName.AppDir/usr/bin/$appName
    # create AppImage
    ./../appimagetool-$ARCH.AppImage -l $appName.AppDir
    mv $appName-$ARCH.AppImage artifacts/$appName
  fi
done

ls -larth artifacts
cp -r ../artifacts/{smartcont,lib} artifacts/
pwd
ls -larth artifacts
