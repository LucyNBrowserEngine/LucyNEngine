# Turns a binary font file (third_party/fonts/*.ttf) into a C++ byte array, the
# same "vendor the source of truth, generate the binary form" habit as
# modules/js/embed_prelude.cmake does for the JS prelude.
# Args: -DFONT=<path to .ttf> -DSYM=<array symbol name> -DOUT=<generated .inc path>
file(READ "${FONT}" hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," hex "${hex}")
file(WRITE "${OUT}" "// Generated at configure time by embed_font.cmake from ${FONT}. Do not edit by hand.\n")
file(APPEND "${OUT}" "extern const unsigned char ${SYM}[] = {${hex}};\n")
file(APPEND "${OUT}" "extern const unsigned int ${SYM}_len = sizeof(${SYM});\n")
