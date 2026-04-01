#pragma once

#include <iostream>

// -----------------------------------------------------------------------------
// Global logging defaults
// -----------------------------------------------------------------------------
// 0: no logs, 1: errors only, 2: +verbosity-1, 3: +verbosity-2
#ifndef SC_LOG_DEFAULT_LEVEL
#define SC_LOG_DEFAULT_LEVEL 2
#endif

// -----------------------------------------------------------------------------
// Per-submodule log-level configuration
// Override any of these at compile time (e.g. -DSC_LOG_LEVEL_COMMTASK=3).
// -----------------------------------------------------------------------------
#ifndef SC_LOG_LEVEL_COMMTASK
#define SC_LOG_LEVEL_COMMTASK SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_ATTACK
#define SC_LOG_LEVEL_ATTACK SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_SOCKET_OP
#define SC_LOG_LEVEL_SOCKET_OP SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_SOCKET_AT
#define SC_LOG_LEVEL_SOCKET_AT SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_LIBIEC
#define SC_LOG_LEVEL_LIBIEC SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_IECMGR
#define SC_LOG_LEVEL_IECMGR SC_LOG_DEFAULT_LEVEL
#endif

#ifndef SC_LOG_LEVEL_CONTROL
#define SC_LOG_LEVEL_CONTROL SC_LOG_DEFAULT_LEVEL
#endif

// -----------------------------------------------------------------------------
// Color configuration
// -----------------------------------------------------------------------------
// Set to 0 to disable ANSI colors in all logs.
#ifndef SC_LOG_USE_COLOR
#define SC_LOG_USE_COLOR 1
#endif

#if SC_LOG_USE_COLOR
#define SC_LOG_COLOR_RESET "\x1b[0m"
#define SC_LOG_COLOR_ERROR "\x1b[31m"

// Base colors (V1)
#define SC_LOG_COLOR_COMMTASK_V1 "\x1b[36m"   // cyan
#define SC_LOG_COLOR_ATTACK_V1   "\x1b[35m"   // magenta
#define SC_LOG_COLOR_SOCKET_OP_V1 "\x1b[32m"  // green
#define SC_LOG_COLOR_SOCKET_AT_V1 "\x1b[33m"  // yellow
#define SC_LOG_COLOR_LIBIEC_V1   "\x1b[34m"   // blue
#define SC_LOG_COLOR_IECMGR_V1   "\x1b[96m"   // bright cyan
#define SC_LOG_COLOR_CONTROL_V1  "\x1b[92m"   // bright green

// Grayer variants (V2): dimmed version of each module color.
#define SC_LOG_COLOR_COMMTASK_V2 "\x1b[2;36m"
#define SC_LOG_COLOR_ATTACK_V2   "\x1b[2;35m"
#define SC_LOG_COLOR_SOCKET_OP_V2 "\x1b[2;32m"
#define SC_LOG_COLOR_SOCKET_AT_V2 "\x1b[2;33m"
#define SC_LOG_COLOR_LIBIEC_V2   "\x1b[2;34m"
#define SC_LOG_COLOR_IECMGR_V2   "\x1b[2;96m"
#define SC_LOG_COLOR_CONTROL_V2  "\x1b[2;92m"
#else
#define SC_LOG_COLOR_RESET ""
#define SC_LOG_COLOR_ERROR ""

#define SC_LOG_COLOR_COMMTASK_V1 ""
#define SC_LOG_COLOR_ATTACK_V1   ""
#define SC_LOG_COLOR_SOCKET_OP_V1 ""
#define SC_LOG_COLOR_SOCKET_AT_V1 ""
#define SC_LOG_COLOR_LIBIEC_V1   ""
#define SC_LOG_COLOR_IECMGR_V1   ""
#define SC_LOG_COLOR_CONTROL_V1  ""

#define SC_LOG_COLOR_COMMTASK_V2 ""
#define SC_LOG_COLOR_ATTACK_V2   ""
#define SC_LOG_COLOR_SOCKET_OP_V2 ""
#define SC_LOG_COLOR_SOCKET_AT_V2 ""
#define SC_LOG_COLOR_LIBIEC_V2   ""
#define SC_LOG_COLOR_IECMGR_V2   ""
#define SC_LOG_COLOR_CONTROL_V2  ""
#endif

// -----------------------------------------------------------------------------
// Internal helper for compile-time enabled/disabled module logging macros.
// -----------------------------------------------------------------------------
#define SC_LOG_EMIT(stream, tag, msg) \
	do {                              \
		stream << tag << msg << "\n"; \
	} while (0)

#define SC_LOG_EMIT_COLOR(stream, color, tag, msg)   \
	do {                                               \
		stream << color << tag << msg                    \
		       << SC_LOG_COLOR_RESET << "\n";          \
	} while (0)

// -----------------------------------------------------------------------------
// COMMTASK macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_COMMTASK >= 1
#define COMMTASK_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[CommunicationTask][ERR] ", msg)
#else
#define COMMTASK_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_COMMTASK >= 2
#define COMMTASK_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_COMMTASK_V1, "[CommunicationTask] ", msg)
#else
#define COMMTASK_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_COMMTASK >= 3
#define COMMTASK_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_COMMTASK_V2, "[CommunicationTask][V2] ", msg)
#else
#define COMMTASK_LOG_V2(msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// ATTACK macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_ATTACK >= 1
#define ATTACK_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[AT][ERR] ", msg)
#else
#define ATTACK_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_ATTACK >= 2
#define ATTACK_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_ATTACK_V1, "[AT] ", msg)
#else
#define ATTACK_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_ATTACK >= 3
#define ATTACK_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_ATTACK_V2, "[AT][V2] ", msg)
#else
#define ATTACK_LOG_V2(msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// SOCKET operator-server macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_SOCKET_OP >= 1
#define SOCKET_OP_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[OP][ERR] ", msg)
#else
#define SOCKET_OP_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_SOCKET_OP >= 2
#define SOCKET_OP_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_SOCKET_OP_V1, "[OP] ", msg)
#else
#define SOCKET_OP_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_SOCKET_OP >= 3
#define SOCKET_OP_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_SOCKET_OP_V2, "[OP][V2] ", msg)
#else
#define SOCKET_OP_LOG_V2(msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// SOCKET attack-server macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_SOCKET_AT >= 1
#define SOCKET_AT_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[AT][ERR] ", msg)
#else
#define SOCKET_AT_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_SOCKET_AT >= 2
#define SOCKET_AT_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_SOCKET_AT_V1, "[AT] ", msg)
#else
#define SOCKET_AT_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_SOCKET_AT >= 3
#define SOCKET_AT_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_SOCKET_AT_V2, "[AT][V2] ", msg)
#else
#define SOCKET_AT_LOG_V2(msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// libiec_wrapper macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_LIBIEC >= 1
#define LIBIEC_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[libiec_wrapper][ERR] ", msg)
#else
#define LIBIEC_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_LIBIEC >= 2
#define LIBIEC_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_LIBIEC_V1, "[libiec_wrapper] ", msg)
#else
#define LIBIEC_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_LIBIEC >= 3
#define LIBIEC_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_LIBIEC_V2, "[libiec_wrapper][V2] ", msg)
#else
#define LIBIEC_LOG_V2(msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// IEC61850Manager macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_IECMGR >= 1
#define IECMGR_ERR(id, msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[IEC61850][" << (id) << "][ERR] ", msg)
#else
#define IECMGR_ERR(id, msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_IECMGR >= 2
#define IECMGR_LOG_V1(id, msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_IECMGR_V1, "[IEC61850][" << (id) << "] ", msg)
#else
#define IECMGR_LOG_V1(id, msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_IECMGR >= 3
#define IECMGR_LOG_V2(id, msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_IECMGR_V2, "[IEC61850][" << (id) << "][V2] ", msg)
#else
#define IECMGR_LOG_V2(id, msg) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// ControlTask macros
// -----------------------------------------------------------------------------
#if SC_LOG_LEVEL_CONTROL >= 1
#define CONTROL_ERR(msg) SC_LOG_EMIT_COLOR(std::cerr, SC_LOG_COLOR_ERROR, "[ControlTask][ERR] ", msg)
#else
#define CONTROL_ERR(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_CONTROL >= 2
#define CONTROL_LOG_V1(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_CONTROL_V1, "[ControlTask] ", msg)
#else
#define CONTROL_LOG_V1(msg) do {} while (0)
#endif

#if SC_LOG_LEVEL_CONTROL >= 3
#define CONTROL_LOG_V2(msg) SC_LOG_EMIT_COLOR(std::cout, SC_LOG_COLOR_CONTROL_V2, "[ControlTask][V2] ", msg)
#else
#define CONTROL_LOG_V2(msg) do {} while (0)
#endif

