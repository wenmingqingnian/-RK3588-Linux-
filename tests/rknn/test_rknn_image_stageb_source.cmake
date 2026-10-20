file(READ "${STAGEB_SOURCE}" stageb_source)
string(REGEX REPLACE "//[^\r\n]*" "" stageb_code "${stageb_source}")
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" stageb_code
       "${stageb_code}")

string(REGEX MATCHALL "rknn_image::decodeOutputs[ \t\r\n]*\\(" decode_calls
       "${stageb_code}")
list(LENGTH decode_calls decode_call_count)
if(NOT decode_call_count EQUAL 1)
    message(FATAL_ERROR
        "Stage B must call rknn_image::decodeOutputs exactly once; found ${decode_call_count}")
endif()

foreach(forbidden IN ITEMS decodeYoloLayer classAwareNms reverseLetterbox)
    if(stageb_code MATCHES
       "(^|[^A-Za-z0-9_])${forbidden}[ \t\r\n]*\\(")
        message(FATAL_ERROR
            "Stage B must not call low-level ${forbidden} directly")
    endif()
endforeach()

string(REGEX MATCHALL "outputsRelease[ \t\r\n]*\\(" release_owner_calls
       "${stageb_code}")
list(LENGTH release_owner_calls release_owner_call_count)
if(NOT release_owner_call_count EQUAL 1)
    message(FATAL_ERROR
        "Stage B must have exactly one outputsRelease ownership path; found ${release_owner_call_count}")
endif()
if(NOT stageb_code MATCHES "class[ \t\r\n]+OutputsLease")
    message(FATAL_ERROR "Stage B must own successful outputs_get buffers with OutputsLease")
endif()
if(NOT stageb_code MATCHES "outputLease\\.release[ \t\r\n]*\\(")
    message(FATAL_ERROR "Stage B must explicitly release and check outputs on success")
endif()

if(NOT stageb_code MATCHES
   "if[ \t\r\n]*\\([ \t\r\n]*args\\.expectClassExplicit[ \t\r\n]*\\)")
    message(FATAL_ERROR
        "Stage B must handle every explicit --expect-class value before metadata fallback")
endif()
if(NOT stageb_code MATCHES "ERROR=--expect-class must not be empty")
    message(FATAL_ERROR
        "Stage B must reject an explicitly empty --expect-class value")
endif()

if(stageb_source MATCHES "#include[ \t]*\"rknn_image_common\\.h\"")
    message(FATAL_ERROR "Stage B still includes the duplicate rknn_image_common.h")
endif()

if(NOT stageb_source MATCHES
       "metadataPath[ \t]*=[ \t]*\"\\./config/yolov5s_metadata\\.conf\"")
    message(FATAL_ERROR "Stage B does not default to canonical production metadata")
endif()
