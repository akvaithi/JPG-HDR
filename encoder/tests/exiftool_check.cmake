# Reproduces the "Metadata Compliance Audit" acceptance criteria from the build
# spec using exiftool, exactly as a reviewer would run it by hand.
set(TIFF ${TMPDIR}/compliance.tif)
set(JPG ${TMPDIR}/compliance.jpg)

execute_process(COMMAND ${FIXTURE} ${TIFF} 512 384 8.0 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to build the test fixture")
endif()

execute_process(
  COMMAND ${ENCODER} --input ${TIFF} --output ${JPG}
          --headroom 4.0 --color-space DisplayP3 --subsample 2
          --channels mono --quality 90
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "encoder exited with ${rc}")
endif()

# 1. The ISO 21496-1 URN must be present in the file. exiftool -v3 wraps its
# hex dump across lines, so match the bytes rather than the rendered text.
execute_process(COMMAND ${EXIFTOOL} -v3 ${JPG}
                OUTPUT_VARIABLE verbose ERROR_VARIABLE verbose_err
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exiftool -v3 failed: ${verbose_err}")
endif()
if(NOT verbose MATCHES "APP2")
  message(FATAL_ERROR "exiftool -v3 shows no APP2 segments")
endif()
# "urn:iso:std:iso:ts:21496:-1\0" as hex.
set(URN_HEX "75726e3a69736f3a7374643a69736f3a74733a32313439363a2d3100")
file(READ ${JPG} JPG_HEX HEX)
string(FIND "${JPG_HEX}" "${URN_HEX}" urn_pos)
if(urn_pos EQUAL -1)
  message(FATAL_ERROR "the ISO 21496-1 URN is not present in ${JPG}")
endif()

# 2. A valid MPF directory listing exactly two images.
execute_process(COMMAND ${EXIFTOOL} -MPFVersion -NumberOfImages -MPImageType
                        -MPImageLength -MPImageStart ${JPG}
                OUTPUT_VARIABLE mpf RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exiftool failed to read the MPF directory")
endif()
if(NOT mpf MATCHES "MPF Version")
  message(FATAL_ERROR "no MPF version tag found:\n${mpf}")
endif()
if(NOT mpf MATCHES "Number Of Images *: *2")
  message(FATAL_ERROR "expected exactly 2 images in the MPF directory:\n${mpf}")
endif()

# 3. exiftool must be able to extract the second image intact.
execute_process(COMMAND ${EXIFTOOL} -b -MPImage2 ${JPG}
                OUTPUT_FILE ${TMPDIR}/extracted_gainmap.jpg
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exiftool could not extract the gain map image")
endif()
file(SIZE ${TMPDIR}/extracted_gainmap.jpg gainmap_size)
if(gainmap_size LESS 100)
  message(FATAL_ERROR "extracted gain map is implausibly small: ${gainmap_size}")
endif()

# 4. The extracted second image must itself carry the ISO 21496-1 URN, which
# is where the standard says the metadata lives.
file(READ ${TMPDIR}/extracted_gainmap.jpg GAINMAP_HEX HEX)
string(FIND "${GAINMAP_HEX}" "${URN_HEX}" urn_pos2)
if(urn_pos2 EQUAL -1)
  message(FATAL_ERROR "the gain map image does not carry the ISO 21496-1 URN")
endif()

# 5. No structural warnings, and nothing missing that -validate insists on.
execute_process(COMMAND ${EXIFTOOL} -validate -warning -a ${JPG}
                OUTPUT_VARIABLE warnings RESULT_VARIABLE rc)
if(warnings MATCHES "Warning *:")
  message(FATAL_ERROR "exiftool reported warnings:\n${warnings}")
endif()

message(STATUS "exiftool compliance checks passed (gain map ${gainmap_size} bytes)")
