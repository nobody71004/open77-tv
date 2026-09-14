#pragma once

// Running the decoder, and nothing else about it.
//
// ---------------------------------------------------------------------------
// WHY THIS LIVES IN THE WEB HOST AND ONLY SPAWNS WHAT IT NAMES
// ---------------------------------------------------------------------------
//
// A link a player pastes is executed by ffprobe and then by ffmpeg. That is the
// whole risk surface of the transcode feature, and this file is its boundary:
// it spawns exactly two executables, both looked up at an absolute path the
// operator stages, both invoked with one argument per field and never through a
// shell, with a URL that `AcceptSource` has already refused unless it is plain
// `http`/`https`. Everything else -- no `cmd.exe`, no environment passthrough
// beyond what Windows supplies, no working directory of note, no output file --
// is closed on purpose.
//
// The two executables are located exactly once per process, and the location is
// not a search of PATH. A decoder discovered by name on PATH is a decoder an
// attacker with filesystem write access can replace; a decoder at a path the
// web host is started with is not. The game passes `-open77-decoder-dir`; if it
// names a directory without the two tools, the feature reports itself disabled
// rather than falling back to a search.
//
// Every child lives in a Windows Job Object that is closed when the stream
// ends. ffmpeg cannot be reasoned into always exiting -- a network stall, a
// live stream, a killed browser -- and an orphaned encoder burns a core
// forever. `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is the guarantee that a child
// dies with the handle, and the handle dies with this object.

#include <op77/WebUI/TranscodePlan.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace op77::WebUI::Transcode
{
/// The resolved, absolute paths of the two tools. Empty members mean the
/// feature is disabled; `DisabledReason` says why.
struct DecoderPaths
{
    std::filesystem::path probe;
    std::filesystem::path stream;
};

/// Looks for `ffprobe.exe` and `ffmpeg.exe` in one directory and nowhere else.
[[nodiscard]] inline DecoderPaths FindDecoderTools(const std::filesystem::path& aDirectory)
{
    DecoderPaths paths;
    const auto probe = aDirectory / L"ffprobe.exe";
    const auto stream = aDirectory / L"ffmpeg.exe";
    std::error_code error;
    if (std::filesystem::is_regular_file(probe, error)) paths.probe = std::filesystem::weakly_canonical(probe, error);
    if (std::filesystem::is_regular_file(stream, error)) paths.stream = std::filesystem::weakly_canonical(stream, error);
    return paths;
}

[[nodiscard]] inline std::string DisabledReason(const DecoderPaths& aPaths)
{
    if (aPaths.probe.empty() && aPaths.stream.empty()) return "no decoder tools were staged";
    if (aPaths.probe.empty()) return "ffprobe.exe is missing from the staged decoder directory";
    if (aPaths.stream.empty()) return "ffmpeg.exe is missing from the staged decoder directory";
    return {};
}

/// One finished child: what it wrote and how it ended.
struct ProcessResult
{
    std::string output;
    int exitCode = -1;
    bool timedOut = false;
    bool started = false;
};

namespace Detail
{/// Reads a pipe to exhaustion on the calling thread. The child's stderr is
/// bounded this way rather than read-after-exit, because a decoder blocked on a
/// full stderr pipe is a deadlock this design cannot otherwise hit.
[[nodiscard]] inline std::string DrainPipe(HANDLE aPipe)
{
    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(aPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        output.append(buffer, read);
        if (output.size() > 512U * 1024U) break; // ffprobe never writes this much; stop feeding an error loop.
    }
    return output;
}

struct JobOwner
{
    HANDLE job = nullptr;
    ~JobOwner()
    {
        if (job != nullptr) CloseHandle(job);
    }
};

/// A child inside a job that dies with `owner`. Returns the process handle the
/// caller must close, or nullptr.
[[nodiscard]] inline HANDLE StartInJob(const std::string& aCommandLine, HANDLE aStdOut, HANDLE aStdErr,
                                       HANDLE aStdIn, JobOwner& aOwner)
{
    aOwner.job = CreateJobObjectW(nullptr, nullptr);
    if (aOwner.job == nullptr) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(aOwner.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        return nullptr;
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = aStdOut;
    startup.hStdError = aStdErr;
    startup.hStdInput = aStdIn;
    // `-nostdin` is on every argv, and the write end of this pipe is closed in
    // the parent the moment the child starts, so even a ffmpeg that ignores it
    // reads end-of-file rather than waiting on this process forever.
    PROCESS_INFORMATION process{};
    // CreateProcessA may write to its command line buffer (it normalises the
    // program name), so the copy is mutable and local.
    std::string mutableCommandLine = aCommandLine;
    if (!CreateProcessA(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
    {
        return nullptr;
    }
    if (!AssignProcessToJobObject(aOwner.job, process.hProcess))
    {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return nullptr;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    return process.hProcess;
}

/// Waits with a deadline, killing through the job when it expires.
[[nodiscard]] inline ::op77::WebUI::Transcode::ProcessResult WaitLimited(HANDLE aProcess, JobOwner& aOwner,
                                                                        const std::chrono::milliseconds aDeadline)
{
    ProcessResult result;
    const ULONGLONG start = GetTickCount64();
    while (true)
    {
        if (WaitForSingleObject(aProcess, 250) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(aProcess, &code);
            result.exitCode = static_cast<int>(code);
            result.started = true;
            break;
        }
        if (GetTickCount64() - start > static_cast<ULONGLONG>(aDeadline.count()))
        {
            // The job terminates everything in it, including grandchildren.
            TerminateJobObject(aOwner.job, 2);
            WaitForSingleObject(aProcess, 5000);
            result.timedOut = true;
            result.started = true;
            break;
        }
    }
    CloseHandle(aProcess);
    return result;
}
} // namespace Detail

/// Probe deadline: a link that has not answered in a minute is not going to.
inline constexpr std::chrono::milliseconds kProbeDeadline{60'000};

[[nodiscard]] inline ProcessResult RunProbe(const DecoderPaths& aPaths, const std::string_view aUrl)
{
    ProcessResult result;
    std::vector<std::string> arguments{aPaths.probe.string()};
    const auto probeArguments = ProbeArguments(std::string(aUrl));
    arguments.insert(arguments.end(), probeArguments.begin(), probeArguments.end());
    const std::string commandLine = BuildCommandLine(arguments);
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return result;
    // This end must not leak into the child.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // One NUL device serves stderr and stdin: a probe's failure story is not
    // worth a second pipe and the deadlock risk of draining it, because the
    // exit code plus empty stdout already answer the only question the route
    // asks. Stdin must exist as a handle (CREATE_SUSPENDED + std handles) so
    // `-nostdin` has something to point away from.
    SECURITY_ATTRIBUTES inherit3{};
    inherit3.nLength = sizeof(inherit3);
    inherit3.bInheritHandle = TRUE;
    HANDLE nullEnd = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inherit3, OPEN_EXISTING, 0, nullptr);
    if (nullEnd == INVALID_HANDLE_VALUE)
    {
        // An invalid std handle makes CreateProcess refuse the child outright;
        // the NUL device never fails in practice, so this is a sanity exit.
        CloseHandle(writeEnd);
        CloseHandle(readEnd);
        return result;
    }
    Detail::JobOwner job;
    HANDLE process = Detail::StartInJob(commandLine, writeEnd, nullEnd, nullEnd, job);
    CloseHandle(writeEnd);
    CloseHandle(nullEnd);
    if (process == nullptr)
    {
        CloseHandle(readEnd);
        return result;
    }
    const ::op77::WebUI::Transcode::ProcessResult finished = Detail::WaitLimited(process, job, kProbeDeadline);
    result.started = finished.started;
    result.exitCode = finished.exitCode;
    result.timedOut = finished.timedOut;
    result.output = Detail::DrainPipe(readEnd);
    CloseHandle(readEnd);
    return result;
}

/// Everything the stream route needs that is decided before the first byte.
struct StreamStart
{
    bool started = false;
    std::string failure;
    HANDLE readEnd = nullptr;  // caller-owned on success
    HANDLE process = nullptr;  // caller-owned on success
    HANDLE job = nullptr;      // caller-owned on success; closing it kills ffmpeg
};

/// Starts the transcode and returns the pipe its WebM arrives on. The caller
/// owns all three handles: read them until exhaustion, then close them -- the
/// job last, because closing it is what kills a still-running encoder.
[[nodiscard]] inline StreamStart StartStream(const DecoderPaths& aPaths, const std::string_view aUrl,
                                             const double aStartSeconds)
{
    StreamStart start;
    std::vector<std::string> arguments{aPaths.stream.string()};
    const auto streamArguments = StreamArguments(std::string(aUrl), aStartSeconds);
    arguments.insert(arguments.end(), streamArguments.begin(), streamArguments.end());
    const std::string commandLine = BuildCommandLine(arguments);
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
    {
        start.failure = "the transcode pipe could not be created";
        return start;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    SECURITY_ATTRIBUTES inherit3{};
    inherit3.nLength = sizeof(inherit3);
    inherit3.bInheritHandle = TRUE;
    HANDLE nullEnd = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inherit3, OPEN_EXISTING, 0, nullptr);
    if (nullEnd == INVALID_HANDLE_VALUE)
    {
        CloseHandle(writeEnd);
        CloseHandle(readEnd);
        start.failure = "the decoder's null device could not be opened";
        return start;
    }
    auto* owner = new Detail::JobOwner();
    HANDLE process = Detail::StartInJob(commandLine, writeEnd, nullEnd, nullEnd, *owner);
    CloseHandle(writeEnd);
    CloseHandle(nullEnd);
    if (process == nullptr)
    {
        delete owner;
        CloseHandle(readEnd);
        start.failure = "the decoder could not be started";
        return start;
    }
    // A first read is the difference between "started" and "about to fail".
    // ffmpeg writes nothing on success either way, so this only detects the
    // immediate-exit case (bad URL, refused protocol) within a second.
    const DWORD waited = WaitForSingleObject(process, 1000);
    if (waited == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        delete owner;
        CloseHandle(readEnd);
        start.failure = "the decoder exited immediately with code " + std::to_string(code);
        return start;
    }
    start.started = true;
    start.readEnd = readEnd;
    start.process = process;
    start.job = owner->job;
    owner->job = nullptr; // ownership moved to the caller
    delete owner;
    return start;
}
} // namespace op77::WebUI::Transcode
