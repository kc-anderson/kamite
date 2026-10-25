#pragma once

// Port log: "[  12.3456][BOOT] message" lines, Info to stdout, Warn/Error to stderr.
// GENERIC: adapted from the Urban Reign port's src/urban/log.h (namespace, macro prefix and env name renamed, the Dll
// category added).

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace kamite::log
{
    enum class Category : uint32_t
    {
        Boot = 1u << 0,
        Loader = 1u << 1,
        File = 1u << 2,
        Ee = 1u << 3,
        Iop = 1u << 4,
        Gs = 1u << 5,
        Vu = 1u << 6,
        Spu2 = 1u << 7,
        Input = 1u << 8,
        Game = 1u << 9,
        Hook = 1u << 10,
        Dll = 1u << 11, // SNR2 DLL loads (function_hooks.cpp, installDllLoadLogger)
    };

    enum class Level
    {
        Error,
        Warn,
        Info,
        Trace,
    };

    // KAMITE_TRACE=BOOT,LOADER,FILE,EE,IOP,GS,VU,SPU2,INPUT,GAME,HOOK,DLL|ALL: categories whose Trace lines print.
    void configureFromEnvironment();
    void setTraceMask(uint32_t mask);
    bool traceEnabled(Category category);
    void write(Category category, Level level, std::string_view message);
    // Seconds since the log started: the time stamp of every log line ("[  12.3456]").
    double elapsedSeconds();
    std::string_view name(Category category);
}

#define KM_LOG(cat, lvl, expr)                                                      \
    do                                                                             \
    {                                                                              \
        if ((lvl) != ::kamite::log::Level::Trace || ::kamite::log::traceEnabled(cat)) \
        {                                                                          \
            std::ostringstream kmLogStream_;                                       \
            kmLogStream_ << expr;                                                  \
            ::kamite::log::write((cat), (lvl), kmLogStream_.str());               \
        }                                                                          \
    } while (0)

#define KM_INFO(cat, expr) KM_LOG(::kamite::log::Category::cat, ::kamite::log::Level::Info, expr)
#define KM_WARN(cat, expr) KM_LOG(::kamite::log::Category::cat, ::kamite::log::Level::Warn, expr)
#define KM_ERROR(cat, expr) KM_LOG(::kamite::log::Category::cat, ::kamite::log::Level::Error, expr)
#define KM_TRACE(cat, expr) KM_LOG(::kamite::log::Category::cat, ::kamite::log::Level::Trace, expr)

// Aliases for code written against the Urban-style names (port_prep/hle-bindings uses KAMITE_*).
#define KAMITE_INFO(cat, expr) KM_INFO(cat, expr)
#define KAMITE_WARN(cat, expr) KM_WARN(cat, expr)
#define KAMITE_ERROR(cat, expr) KM_ERROR(cat, expr)
#define KAMITE_TRACE(cat, expr) KM_TRACE(cat, expr)
