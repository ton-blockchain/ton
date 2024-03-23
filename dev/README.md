# Core Developer Documentation

The content of this folder affects developers that work on the TON source-code itself.

## dot

[dot](dot) contains graphviz dot-files generated via cmake.

```sh
# recreate the files on an ubuntu machine
# note: cmake does not add a .dot extension

sudo apt install dot graphviz

cd ~/ton

# generate graphviz dot files
cmake  --graphviz=./dev/dot/ton -B build

# create .png files from .dot files
find ./dev/dot -type f -name "ton.*" | xargs dot -Tpng -O

mv ./dev/dot/*.png ./dev/png/

# add .dot extension
for f in dev/dot/ton*; do mv $f $f.dot; done
```

Configure the graphviz output via [CMakeGraphVizOptions.cmake](../CMakeGraphVizOptions.cmake).

