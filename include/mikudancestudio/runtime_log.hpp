#pragma once

#include <cstdarg>

namespace mikudancestudio::runtime_log {
// Runtime diagnostics for the shipped build.  Always enabled, independently
// of the porting-era DIAG probes. The newest messages stay in a bounded
// in-memory ring. A fatal fault saves error-*.log in data/backups beside the actual
// executable and shows a modal report. Project recovery has its own callback.
// The process only ends after the user presses OK.
void Initialize() noexcept;
// Optional application-owned, best-effort recovery before showing a fatal error.
// Diagnostics itself remains independent of project serialization.
using CrashBackup = void (*)(unsigned long exceptionCode) noexcept;
void SetCrashBackup(CrashBackup callback) noexcept;
void Write(const char* format, ...) noexcept;
void WritePath(const char* label, const wchar_t* path) noexcept;
// The phase must be a string literal (no allocation or application-state
// dereference is needed to retrieve it from the fault handler).
void SetPhase(const char* phase) noexcept;
void Heartbeat(int frame) noexcept;
// Optional append-only diagnostic channel for the hardening guards: the line is
// written to the path named by MIKUDANCESTUDIO_TRACE_FILE, and nothing at all
// happens - no file, no directory, no descriptor - when that variable is unset
// or empty.  A normal run therefore still creates nothing on disk; the
// variable exists only for field diagnostics.
void Trace(const char* format, ...) noexcept;
void TraceV(const char* format, va_list args) noexcept;
// Marks the process as exiting (the user confirmed the close / the message
// loop ended).  A fault from here on is a teardown fault: the window is gone
// and the settings are already written, so it is recorded silently instead of
// raising the modal report - closing the program normally must not end in an
// error box.
void BeginShutdown() noexcept;
void NormalExit(int code) noexcept;
// Records `reason` and stops the process: the modal last-error window is
// shown first, and the process terminates only once it is acknowledged.
// Use for startup failures that would otherwise exit without any report.
[[noreturn]] void Fatal(const char* reason) noexcept;
}
