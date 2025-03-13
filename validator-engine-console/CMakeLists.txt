add_executable (validator-engine-console validator-engine-console.cpp
  validator-engine-console.h validator-engine-console-query.cpp
  validator-engine-console-query.h )
target_link_libraries(validator-engine-console tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_crypto terminal git)

install(TARGETS validator-engine-console RUNTIME DESTINATION bin)

