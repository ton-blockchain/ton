## 2024.02 Update

1. Improvement of validator synchronisation:
   * Better handling of block broadcasts -> faster sync
   * Additional separate overlay among validators as second option for synchronisation
2. Improvements in LS:
   * c7 and library context is fully filled up for server-side rungetmethod
   * Cache for runmethods and successfull external messages
   * Logging of LS requests statistic
3. Precise control of open files:
   * almost instantaneous validator start
   * `--max-archive-fd` option
   * autoremoval of not used temp archive files
   * `--archive-preload-period` option
4. Preparatory (not enabled yet) code for addition on new TVM instructions for cheaper fee calculation onchain.


