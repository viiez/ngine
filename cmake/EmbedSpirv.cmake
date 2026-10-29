# Wandelt eine SPIR-V-Datei in eine Liste von 32-Bit-Wörtern für C++ um.
# Aufruf: cmake -DINPUT=<datei.spv> -DOUTPUT=<datei.inc> -P EmbedSpirv.cmake

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR remainder "${length} % 8")
if(length EQUAL 0 OR NOT remainder EQUAL 0)
    message(FATAL_ERROR "${INPUT} ist kein gültiges SPIR-V (Größe ist kein Vielfaches von 4 Byte)")
endif()

string(REGEX MATCHALL "........" words "${hex}")
set(content "")
set(column 0)
foreach(word IN LISTS words)
    # SPIR-V liegt als Little Endian vor: Bytes umdrehen.
    string(SUBSTRING "${word}" 0 2 b0)
    string(SUBSTRING "${word}" 2 2 b1)
    string(SUBSTRING "${word}" 4 2 b2)
    string(SUBSTRING "${word}" 6 2 b3)
    string(APPEND content "0x${b3}${b2}${b1}${b0},")
    math(EXPR column "${column} + 1")
    if(column EQUAL 8)
        string(APPEND content "\n")
        set(column 0)
    endif()
endforeach()

file(WRITE "${OUTPUT}" "${content}\n")
