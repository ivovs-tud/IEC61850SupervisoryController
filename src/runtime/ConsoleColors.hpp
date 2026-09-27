#pragma once

// -----------------------------------------------------------------------------
// Platform-specific console color initialization
// This file should be included ONLY in main.cpp to avoid header conflicts
// -----------------------------------------------------------------------------

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    
    // Enable virtual terminal processing on Windows 10+ for ANSI color support
    inline bool enableWindowsConsoleColors() {
        static bool initialized = false;
        static bool success = false;
        
        if (!initialized) {
            initialized = true;
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            
            if (hOut != INVALID_HANDLE_VALUE) {
                DWORD dwMode = 0;
                if (GetConsoleMode(hOut, &dwMode)) {
                    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    if (SetConsoleMode(hOut, dwMode)) {
                        success = true;
                    }
                }
            }
            
            if (hErr != INVALID_HANDLE_VALUE) {
                DWORD dwMode = 0;
                if (GetConsoleMode(hErr, &dwMode)) {
                    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    SetConsoleMode(hErr, dwMode);
                }
            }
        }
        
        return success;
    }
#else
    // On Linux/Unix, ANSI colors work by default
    inline bool enableWindowsConsoleColors() {
        return true;
    }
#endif
