/* PUBLIC demo image-encryption (ChaCha20) key. It sat in bootloader.c for a while, so anyone
   reading this repository knows it: encrypted images built with it are not confidential. A real
   product builds with its own key (CMake: -DBL_ENC_KEY_HEADER=path/to/key.h), where the header
   defines BL_ENC_KEY_BYTES exactly like this one.
   Generate a key and its header with: python host/sign_tool.py genenckey */
#define BL_ENC_KEY_BYTES { \
    0x28, 0xF1, 0x1D, 0xFA, 0xA1, 0x72, 0x28, 0x9C, \
    0x72, 0x1E, 0xF3, 0xF0, 0xD3, 0xB1, 0x98, 0xF6, \
    0x4A, 0xE3, 0xE3, 0x8F, 0xE5, 0x5E, 0x1D, 0x6C, \
    0x2C, 0x4F, 0x8A, 0x4F, 0x74, 0xD4, 0x06, 0xEA }
