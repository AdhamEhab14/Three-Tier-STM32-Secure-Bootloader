/* PUBLIC demo SecurityAccess key. Anyone reading this repository knows it, so a real
   product must build with its own key (CMake: -DBL_SEC_KEY_HEADER=path/to/key.h), where
   the header defines BL_SEC_KEY_BYTES exactly like this one.
   Generate a key and its header with: python host/sign_tool.py genseckey */
#define BL_SEC_KEY_BYTES { \
    0x44, 0x65, 0x6D, 0x6F, 0x2D, 0x4B, 0x65, 0x79, \
    0x2D, 0x50, 0x75, 0x62, 0x6C, 0x69, 0x63, 0x21 }
