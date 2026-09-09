if(NOT DEFINED DLL_PATH OR NOT EXISTS "${DLL_PATH}")
    message(FATAL_ERROR "DLL_PATH must name an existing QuickGadgets DLL.")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(STAGING_DIR "${OUTPUT_DIR}/staging")
file(REMOVE_RECURSE "${STAGING_DIR}")
file(MAKE_DIRECTORY "${STAGING_DIR}")

file(COPY "${DLL_PATH}" DESTINATION "${STAGING_DIR}")
file(COPY "${SOURCE_DIR}/QuickGadgets.ini" DESTINATION "${STAGING_DIR}")
file(WRITE "${STAGING_DIR}/info.json" "{\n
  \"name\": \"Quick Gadgets\",\n
  \"type\": \"script\",\n
  \"author\": \"Quick Gadgets contributors\",\n
  \"version\": \"${SCRIPT_VERSION}\",\n
  \"dependencies\": [],\n
  \"game\": \"MSMR\",\n
  \"format_version\": 1\n
}\n")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "${OUTPUT_DIR}/QuickGadgets.zip" --format=zip -- info.json QuickGadgets.dll QuickGadgets.ini
    WORKING_DIRECTORY "${STAGING_DIR}"
    RESULT_VARIABLE PACKAGE_RESULT
)
if(NOT PACKAGE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create script archive.")
endif()
file(RENAME "${OUTPUT_DIR}/QuickGadgets.zip" "${OUTPUT_DIR}/QuickGadgets.script")
file(REMOVE_RECURSE "${STAGING_DIR}")
