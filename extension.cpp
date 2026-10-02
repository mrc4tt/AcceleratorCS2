#include "extension.h"

#if defined _LINUX
#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#include "common/linux/http_upload.h"

#include <dirent.h>
#include <unistd.h>
#else
#include "client/windows/handler/exception_handler.h"
#include "common/windows/http_upload.h"
#endif

#include "vendor/nlohmann/json.hpp"

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <limits>
#include <filesystem>
#include <codecvt>
#include <thread>
#include <vector>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <cinttypes>
#include <cctype>

#include "common/path_helper.h"
#include "common/using_std_string.h"
#include "google_breakpad/processor/basic_source_line_resolver.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_state.h"
#include "processor/simple_symbol_supplier.h"
#include "processor/stackwalk_common.h"
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <google_breakpad/processor/stack_frame_cpu.h>
#include <google_breakpad/processor/code_modules.h>
#include <google_breakpad/processor/minidump.h>
#include <processor/pathname_stripper.h>

#include <entity2/entitysystem.h>
#if defined WIN32
#include <corecrt_io.h>
#endif

#include "presubmit.h"

AcceleratorCS2 g_AcceleratorCS2;
PLUGIN_EXPOSE(AcceleratorCS2, g_AcceleratorCS2);

char crashMap[256];
char crashGamePath[512];
char crashCommandLine[1024];
char dumpStoragePath[512];
std::string g_serverId;
std::string g_UserId;
bool g_UploadCrashDumps = false;
bool g_IgnoreShutdownCrashes = true;

// Crash-loop guard: once this many dumps were written within the window, stop writing new ones.
// 0 in either value disables the guard.
int g_CrashLoopMaxDumps = 5;
int g_CrashLoopWindowMinutes = 10;

// Timestamps of dumps already on disk, collected at load so the crash handler never has to touch
// the filesystem to make its decision.
constexpr int kMaxRecentDumps = 64;
time_t g_RecentDumpTimes[kMaxRecentDumps];
int g_NumRecentDumps = 0;

// Set once the server has been asked to stop (quit command, SIGTERM/SIGINT, engine shutdown).
// Crashes after that point are teardown noise from a stop/restart, not real crashes.
volatile sig_atomic_t g_ShuttingDown = 0;

// Extra context for the .dmp.txt. All of it is formatted ahead of time (at load, map start, or when a
// command runs) so the crash handler only has to copy bytes.
time_t g_LoadTime = 0;
char crashPluginList[8192];

constexpr int kCommandHistorySize = 16;
constexpr int kCommandHistoryLength = 192;
char g_CommandHistory[kCommandHistorySize][kCommandHistoryLength];
volatile sig_atomic_t g_CommandHistoryNext = 0;

// Written instead of a stack walk when a dump can't be processed at the next start either, so it
// isn't retried on every boot.
static const char kStackwalkFailedMarker[] = "-------- STACKWALK FAILED --------";

CGameEntitySystem *GameEntitySystem()
{
	return nullptr;
}

class GameSessionConfiguration_t { };
#if defined _LINUX
KHook::Virtual<IServerGameDLL, void, bool, bool, bool> gameFrameHook(&IServerGameDLL::GameFrame, &g_AcceleratorCS2, nullptr, &AcceleratorCS2::GameFrame);
#endif
KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t&, ISource2WorldSession*, const char*> startupServerHook(&INetworkServerService::StartupServer, &g_AcceleratorCS2, nullptr, &AcceleratorCS2::StartupServer);
KHook::Virtual<IServerGameDLL, void> preShutdownHook(static_cast<void (IServerGameDLL::*)()>(&IServerGameDLL::PreShutdown), &g_AcceleratorCS2, &AcceleratorCS2::PreShutdown, nullptr);
KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&> dispatchConCommandHook(&ICvar::DispatchConCommand, &g_AcceleratorCS2, &AcceleratorCS2::DispatchConCommand, nullptr);

google_breakpad::ExceptionHandler* exceptionHandler = nullptr;

void signal_safe_hex_print(uint64_t num)
{
	if (num > 15) {
		signal_safe_hex_print(num / 16);
	}
	char c = "0123456789ABCDEF"[num % 16];
#if defined _LINUX
	sys_write(STDOUT_FILENO, &c, 1);
#else
	write(fileno(stdout), &c, 1);
#endif
}

static bool ShouldSkipDump()
{
	return g_IgnoreShutdownCrashes && g_ShuttingDown;
}

static int CountRecentDumps()
{
	time_t cutoff = time(nullptr) - static_cast<time_t>(g_CrashLoopWindowMinutes) * 60;
	int recent = 0;
	for (int i = 0; i < g_NumRecentDumps; ++i)
	{
		if (g_RecentDumpTimes[i] >= cutoff)
			recent++;
	}
	return recent;
}

static bool IsCrashLooping()
{
	if (g_CrashLoopMaxDumps <= 0 || g_CrashLoopWindowMinutes <= 0)
		return false;

	return CountRecentDumps() >= g_CrashLoopMaxDumps;
}

// Signal-safe decimal formatting, buffer must hold at least 21 chars.
static const char* FormatUInt(char* buffer, uint64_t value)
{
	char* p = buffer + 20;
	*p = '\0';
	do {
		*--p = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value);
	return p;
}

// Runs inside the crash handler: only reads pre-formatted buffers, no allocation.
template <typename Write>
static void WriteCrashContext(Write write)
{
	char number[21];

	write("-------- CONTEXT BEGIN --------\n");
	write("Uptime=");
	write(FormatUInt(number, g_LoadTime ? static_cast<uint64_t>(time(nullptr) - g_LoadTime) : 0));
	write("s\n");

	write("Plugins:\n");
	write(crashPluginList[0] ? crashPluginList : "  (none found)\n");

	write("Recent console commands (oldest first):\n");
	int next = g_CommandHistoryNext;
	for (int i = 0; i < kCommandHistorySize; ++i)
	{
		const char* entry = g_CommandHistory[(next + i) % kCommandHistorySize];
		if (!entry[0])
			continue;
		write("  ");
		write(entry);
		write("\n");
	}
	write("-------- CONTEXT END --------\n\n");
}

static bool ContainsNoCase(const char* haystack, const char* needle)
{
	for (; *haystack; ++haystack)
	{
		const char* h = haystack;
		const char* n = needle;
		while (*h && *n && tolower(static_cast<unsigned char>(*h)) == tolower(static_cast<unsigned char>(*n)))
			++h, ++n;
		if (!*n)
			return true;
	}
	return false;
}

static void RecordCommand(const char* name, const CCommand& args)
{
	static const char* const kSensitive[] = { "pass", "rcon", "token", "key", "secret", "auth" };

	bool sensitive = false;
	for (const char* word : kSensitive)
		sensitive = sensitive || ContainsNoCase(name, word);

	int slot = g_CommandHistoryNext % kCommandHistorySize;
	long uptime = static_cast<long>(time(nullptr) - g_LoadTime);
	if (sensitive)
		V_snprintf(g_CommandHistory[slot], kCommandHistoryLength, "[+%lds] %s <args hidden>", uptime, name);
	else
		V_snprintf(g_CommandHistory[slot], kCommandHistoryLength, "[+%lds] %s", uptime, args.GetCommandString());
	g_CommandHistoryNext = (slot + 1) % kCommandHistorySize;
}

// CounterStrikeSharp plugins are loaded from memory, so they never show up in the module list of a dump.
static void BuildPluginList()
{
	std::string list;
	std::error_code ec;
	const std::filesystem::path pluginDir = std::filesystem::path(crashGamePath) / "addons" / "counterstrikesharp" / "plugins";

	for (std::filesystem::directory_iterator it(pluginDir, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code entryError;
		if (!it->is_directory(entryError))
			continue;

		const std::string name = it->path().filename().string();
		if (name == "disabled")
			continue;

		const std::filesystem::path dll = it->path() / (name + ".dll");
		char line[512];
		struct stat st;
		if (stat(dll.string().c_str(), &st) == 0)
		{
			char when[32] = "?";
			strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", gmtime(&st.st_mtime));
			V_snprintf(line, sizeof(line), "  css/%s (%lld bytes, %s)\n", name.c_str(), static_cast<long long>(st.st_size), when);
		}
		else
		{
			V_snprintf(line, sizeof(line), "  css/%s (no %s.dll)\n", name.c_str(), name.c_str());
		}

		if (list.size() + strlen(line) >= sizeof(crashPluginList) - 1)
			break;
		list += line;
	}

	strncpy(crashPluginList, list.c_str(), sizeof(crashPluginList) - 1);
	crashPluginList[sizeof(crashPluginList) - 1] = '\0';
}

// The bytes at the crashing instruction, from the memory breakpad saves around the IP. Also calls out
// compiler-generated traps: their code is often placed right before the entry point of the function
// that branched to it, so symbolizers attribute the crash to the preceding function.
static void WriteInstructionBytes(FILE* out, google_breakpad::Minidump& dump, uint64_t ip)
{
	google_breakpad::MinidumpMemoryList* memoryList = dump.GetMemoryList();
	google_breakpad::MinidumpMemoryRegion* region = memoryList ? memoryList->GetMemoryRegionForAddress(ip) : nullptr;
	if (!region)
		return;

	uint8_t bytes[16];
	int count = 0;
	for (; count < static_cast<int>(sizeof(bytes)); ++count)
	{
		if (!region->GetMemoryAtAddress(ip + count, &bytes[count]))
			break;
	}
	if (!count)
		return;

	fprintf(out, "Crash IP bytes: ");
	for (int i = 0; i < count; ++i)
		fprintf(out, "%02x ", bytes[i]);
	fprintf(out, "\n");

	static const uint8_t kNullDerefTrap[] = { 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0b }; // mov rax, [0]; ud2
	if (count >= static_cast<int>(sizeof(kNullDerefTrap)) && !memcmp(bytes, kNullDerefTrap, sizeof(kNullDerefTrap)))
		fprintf(out, "Note: compiler-generated null dereference trap (mov rax,[0]; ud2). The code that branched here used a null pointer; this block usually sits in front of that function's entry point.\n");
	else if (count >= 2 && bytes[0] == 0x0f && bytes[1] == 0x0b)
		fprintf(out, "Note: ud2 trap (__builtin_trap / unreachable code reached).\n");
	fprintf(out, "\n");
}

static void WriteFrames(FILE* out, const google_breakpad::CallStack* stack, size_t maxFrames, bool registers)
{
	size_t frameCount = (std::min)(stack->frames()->size(), maxFrames);
	for (size_t i = 0; i < frameCount; ++i)
	{
		const google_breakpad::StackFrame* frame = stack->frames()->at(i);
		uint64_t address = frame->ReturnAddress();
		if (frame->module)
			fprintf(out, "%2zu  %s + 0x%" PRIx64, i, google_breakpad::PathnameStripper::File(frame->module->code_file()).c_str(), address - frame->module->base_address());
		else
			fprintf(out, "%2zu  0x%" PRIx64, i, address);
		fprintf(out, "  (%s)\n", frame->trust_description().c_str());

		if (i == 0 && registers)
		{
			const auto* amd64 = static_cast<const google_breakpad::StackFrameAMD64*>(frame);
			const MDRawContextAMD64& c = amd64->context;
			fprintf(out, "    rax = 0x%016" PRIx64 "   rdx = 0x%016" PRIx64 "   rcx = 0x%016" PRIx64 "   rbx = 0x%016" PRIx64 "\n", c.rax, c.rdx, c.rcx, c.rbx);
			fprintf(out, "    rsi = 0x%016" PRIx64 "   rdi = 0x%016" PRIx64 "   rbp = 0x%016" PRIx64 "   rsp = 0x%016" PRIx64 "\n", c.rsi, c.rdi, c.rbp, c.rsp);
			fprintf(out, "     r8 = 0x%016" PRIx64 "    r9 = 0x%016" PRIx64 "   r10 = 0x%016" PRIx64 "   r11 = 0x%016" PRIx64 "\n", c.r8, c.r9, c.r10, c.r11);
			fprintf(out, "    r12 = 0x%016" PRIx64 "   r13 = 0x%016" PRIx64 "   r14 = 0x%016" PRIx64 "   r15 = 0x%016" PRIx64 "\n", c.r12, c.r13, c.r14, c.r15);
			fprintf(out, "    rip = 0x%016" PRIx64 "\n", c.rip);
		}
	}
}

// Stand-in for breakpad's PrintProcessState that writes to a FILE instead of stdout, so it can run
// on a background thread while the server is up.
static void WriteProcessState(FILE* out, const google_breakpad::ProcessState& state, google_breakpad::Minidump& dump)
{
	const google_breakpad::SystemInfo* info = state.system_info();
	bool amd64 = info && info->cpu == "amd64";
	if (info)
		fprintf(out, "Operating system: %s %s\nCPU: %s %s\n\n", info->os.c_str(), info->os_version.c_str(), info->cpu.c_str(), info->cpu_info.c_str());

	if (state.crashed())
		fprintf(out, "Crash reason:  %s\nCrash address: 0x%" PRIx64 "\n\n", state.crash_reason().c_str(), state.crash_address());
	else
		fprintf(out, "No crash\n\n");

	int requestingThread = state.requesting_thread();
	const auto* threads = state.threads();
	if (requestingThread >= 0 && requestingThread < static_cast<int>(threads->size()))
	{
		const google_breakpad::CallStack* stack = threads->at(requestingThread);
		fprintf(out, "Thread %d (%s)\n", requestingThread, state.crashed() ? "crashed" : "requesting");
		WriteFrames(out, stack, 64, amd64);
		fprintf(out, "\n");
		if (amd64 && !stack->frames()->empty())
			WriteInstructionBytes(out, dump, stack->frames()->at(0)->instruction);
	}

	for (int i = 0; i < static_cast<int>(threads->size()); ++i)
	{
		if (i == requestingThread)
			continue;
		fprintf(out, "Thread %d\n", i);
		WriteFrames(out, threads->at(i), 24, false);
		fprintf(out, "\n");
	}

	const google_breakpad::CodeModules* modules = state.modules();
	if (modules)
	{
		fprintf(out, "Loaded modules:\n");
		for (unsigned int i = 0; i < modules->module_count(); ++i)
		{
			const google_breakpad::CodeModule* module = modules->GetModuleAtIndex(i);
			fprintf(out, "0x%" PRIx64 " - 0x%" PRIx64 "  %s  (%s)\n", module->base_address(), module->base_address() + module->size() - 1,
				google_breakpad::PathnameStripper::File(module->code_file()).c_str(), module->debug_identifier().c_str());
		}
	}
}

#if defined _LINUX
void (*SignalHandler)(int, siginfo_t*, void*);
const int kExceptionSignals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
const int kNumHandledSignals = std::size(kExceptionSignals);

// docker stop / Pterodactyl kill send SIGTERM, Ctrl+C sends SIGINT, a closed screen/tmux sends SIGHUP.
const int kShutdownSignals[] = { SIGTERM, SIGINT, SIGHUP };
const int kNumShutdownSignals = std::size(kShutdownSignals);
struct sigaction oldShutdownActions[kNumShutdownSignals];

static void ShutdownSignalHandler(int sig, siginfo_t* info, void* ucontext)
{
	g_ShuttingDown = 1;

	// Chain to whatever was installed before us so the engine still shuts down the way it normally would.
	for (int i = 0; i < kNumShutdownSignals; ++i)
	{
		if (kShutdownSignals[i] != sig)
			continue;

		const struct sigaction& old = oldShutdownActions[i];
		if (old.sa_flags & SA_SIGINFO)
		{
			if (old.sa_sigaction)
				old.sa_sigaction(sig, info, ucontext);
		}
		else if (old.sa_handler == SIG_DFL)
		{
			// Signal is blocked while we run, so the re-raise is delivered with the default action once we return.
			sigaction(sig, &old, NULL);
			raise(sig);
		}
		else if (old.sa_handler != SIG_IGN)
		{
			old.sa_handler(sig);
		}
		return;
	}
}

static void InstallShutdownSignalHandlers()
{
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);
	act.sa_sigaction = ShutdownSignalHandler;
	act.sa_flags = SA_SIGINFO | SA_RESTART;

	for (int i = 0; i < kNumShutdownSignals; ++i)
		sigaction(kShutdownSignals[i], &act, &oldShutdownActions[i]);
}

static void RemoveShutdownSignalHandlers()
{
	struct sigaction cur;
	for (int i = 0; i < kNumShutdownSignals; ++i)
	{
		// Only restore if nobody replaced us in the meantime.
		if (sigaction(kShutdownSignals[i], NULL, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == ShutdownSignalHandler)
			sigaction(kShutdownSignals[i], &oldShutdownActions[i], NULL);
	}
}

static bool filterCallback(void* context)
{
	if (ShouldSkipDump())
	{
		static const char msg[] = "Accelerator: crash during server shutdown, not writing minidump\n";
		sys_write(STDOUT_FILENO, msg, sizeof(msg) - 1);
		return false;
	}

	if (IsCrashLooping())
	{
		static const char msg[] = "Accelerator: crash loop detected (CrashLoopMaxDumps reached), not writing minidump\n";
		sys_write(STDOUT_FILENO, msg, sizeof(msg) - 1);
		return false;
	}

	return true;
}

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	if (succeeded)
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	else
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded)
		return succeeded;

	my_strlcpy(dumpStoragePath, descriptor.path(), sizeof(dumpStoragePath));
	my_strlcat(dumpStoragePath, ".txt", sizeof(dumpStoragePath));

	int extra = sys_open(dumpStoragePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (extra == -1)
	{
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);
	sys_write(extra, "\n", 1);

	WriteCrashContext([extra](const char* text) { sys_write(extra, text, my_strlen(text)); });

	// Everything below allocates. After heap corruption (glibc "double free or corruption" aborts)
	// it can fail, the stack walk is then filled in on the next start by RepairIncompleteMetadata().
	google_breakpad::scoped_ptr<google_breakpad::SimpleSymbolSupplier> symbolSupplier;
	google_breakpad::BasicSourceLineResolver resolver;
	google_breakpad::MinidumpProcessor minidump_processor(symbolSupplier.get(), &resolver);

	// Increase the maximum number of threads and regions.
	google_breakpad::MinidumpThreadList::set_max_threads(std::numeric_limits<uint32_t>::max());
	google_breakpad::MinidumpMemoryList::set_max_regions(std::numeric_limits<uint32_t>::max());
	// Process the minidump.
	google_breakpad::Minidump miniDump(descriptor.path());
	if (!miniDump.Read())
	{
		sys_write(STDOUT_FILENO, "Failed to read minidump\n", 24);
	}
	else
	{
		google_breakpad::ProcessState processState;
		if (minidump_processor.Process(&miniDump, &processState) != google_breakpad::PROCESS_OK)
		{
			sys_write(STDOUT_FILENO, "MinidumpProcessor::Process failed\n", 34);
		}
		else
		{
			int requestingThread = processState.requesting_thread();
			if (requestingThread == -1) {
				requestingThread = 0;
			}

			const google_breakpad::CallStack* stack = processState.threads()->at(requestingThread);
			int frameCount = stack->frames()->size();
			if (frameCount > 15) {
				frameCount = 15;
			}

			//std::ostringstream stream;

			sys_write(STDOUT_FILENO, "\n", 1);
			for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
				auto frame = stack->frames()->at(frameIndex);

				auto moduleOffset = frame->ReturnAddress();
				if (frame->module) {
					auto moduleFile = google_breakpad::PathnameStripper::File(frame->module->code_file());
					moduleOffset -= frame->module->base_address();
					sys_write(STDOUT_FILENO, moduleFile.c_str(), moduleFile.size());
					sys_write(STDOUT_FILENO, " (0x", 4);
					signal_safe_hex_print(moduleOffset);
					sys_write(STDOUT_FILENO, ")\n", 2);
				}
				else {
					sys_write(STDOUT_FILENO, "unknown (0x", 11);
					signal_safe_hex_print(moduleOffset);
					sys_write(STDOUT_FILENO, ") \n", 3);
				}
			}

			//sys_write(STDOUT_FILENO, stream.str().c_str(), stream.str().length());

			freopen(dumpStoragePath, "a", stdout);
			PrintProcessState(processState, true, false, &resolver);
			if (stack->frames()->size() > 0)
				WriteInstructionBytes(stdout, miniDump, stack->frames()->at(0)->instruction);
			fflush(stdout);
		}
	}

	sys_close(extra);

	return succeeded;
}
#else
void* vectoredHandler = NULL;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	// Ctrl+C, closing the console window, logoff and system shutdown all mean the server is going away.
	g_ShuttingDown = 1;
	return FALSE;
}

static bool filterCallback(void* context, EXCEPTION_POINTERS* exinfo, MDRawAssertionInfo* assertion)
{
	if (ShouldSkipDump())
	{
		printf("Accelerator: crash during server shutdown, not writing minidump\n");
		return false;
	}

	if (IsCrashLooping())
	{
		printf("Accelerator: crash loop detected (CrashLoopMaxDumps reached), not writing minidump\n");
		return false;
	}

	return true;
}

LONG CALLBACK BreakpadVectoredHandler(_In_ PEXCEPTION_POINTERS ExceptionInfo)
{
	switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_INVALID_HANDLE:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
	case 0xC0000374: // STATUS_HEAP_CORRUPTION
		break;
	case 0: // Valve use this for Sys_Error.
		if ((ExceptionInfo->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0)
			return EXCEPTION_CONTINUE_SEARCH;
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exceptionHandler->WriteMinidumpForException(ExceptionInfo))
	{
		// Stop the handler thread from deadlocking us.
		delete exceptionHandler;

		// Stop Valve's handler being called.
		ExceptionInfo->ExceptionRecord->ExceptionCode = EXCEPTION_BREAKPOINT;

		return EXCEPTION_EXECUTE_HANDLER;
	}
	else {
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

std::string ws2s(const std::wstring& wstr)
{
	using convert_typeX = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_typeX, wchar_t> converterX;

	return converterX.to_bytes(wstr);
}

static bool dumpCallback(const wchar_t* dump_path,
	const wchar_t* minidump_id,
	void* context,
	EXCEPTION_POINTERS* exinfo,
	MDRawAssertionInfo* assertion,
	bool succeeded)
{
	if (!succeeded) {
		printf("Failed to write minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);
		return succeeded;
	}

	sprintf(dumpStoragePath, "%ls\\%ls.dmp.txt", dump_path, minidump_id);

	FILE* extra = fopen(dumpStoragePath, "wb");
	if (!extra) {
		printf("Failed to open metadata file!\n");
		return succeeded;
	}

	fprintf(extra, "-------- CONFIG BEGIN --------");
	fprintf(extra, "\nMap=%s", crashMap);
	fprintf(extra, "\nGamePath=%s", crashGamePath);
	fprintf(extra, "\nCommandLine=%s", crashCommandLine);
	fprintf(extra, "\n-------- CONFIG END --------\n");
	fprintf(extra, "\n");

	WriteCrashContext([extra](const char* text) { fputs(text, extra); });
	fflush(extra);

	google_breakpad::scoped_ptr<google_breakpad::SimpleSymbolSupplier> symbolSupplier;
	google_breakpad::BasicSourceLineResolver resolver;
	google_breakpad::MinidumpProcessor minidump_processor(symbolSupplier.get(), &resolver);

	// Increase the maximum number of threads and regions.
#undef max
	google_breakpad::MinidumpThreadList::set_max_threads(std::numeric_limits<uint32_t>::max());
	google_breakpad::MinidumpMemoryList::set_max_regions(std::numeric_limits<uint32_t>::max());
	// Process the minidump.
	std::wstring widestr = std::wstring(dump_path) + L"\\" + std::wstring(minidump_id) + L".dmp";
	google_breakpad::Minidump miniDump(ws2s(widestr));
	if (!miniDump.Read())
	{
		printf("Failed to read minidump\n");
	}
	else
	{
		google_breakpad::ProcessState processState;
		if (minidump_processor.Process(&miniDump, &processState) != google_breakpad::PROCESS_OK)
		{
			printf("MinidumpProcessor::Process failed\n");
		}
		else
		{
			freopen(dumpStoragePath, "a", stdout);
			PrintProcessState(processState, true, false, &resolver);
			int requestingThread = processState.requesting_thread();
			if (requestingThread >= 0 && !processState.threads()->at(requestingThread)->frames()->empty())
				WriteInstructionBytes(stdout, miniDump, processState.threads()->at(requestingThread)->frames()->at(0)->instruction);
			fflush(stdout);
		}
	}

	fclose(extra);

	return succeeded;
}
#endif

#ifdef _LINUX
static void UploadDump(const std::filesystem::path& dumpPath, const std::filesystem::path& metadataPath)
{
	char tokenBuffer[64] = {};
	PresubmitCrashDump(dumpPath.string().c_str(), tokenBuffer, sizeof(tokenBuffer));

	ConMsg("Uploading minidump %s\n", dumpPath.string().c_str());

	std::map<std::string, std::string> params;

	params["UserID"] = g_UserId;
	params["GameDirectory"] = "csgo";
	params["ExtensionVersion"] = std::string(g_AcceleratorCS2.GetVersion()) + " [AcceleratorCS2 Build]";
	params["ServerID"] = g_serverId;

	if (tokenBuffer[0] != '\0')
	{
		params["PresubmitToken"] = tokenBuffer;
	}

	std::map<std::string, std::string> files;
	files["upload_file_minidump"] = dumpPath.string();
	files["upload_file_metadata"] = metadataPath.string();

	std::string res;
	google_breakpad::HTTPUpload::SendRequest("http://crash.limetech.org/submit", params, files, "", "", "", &res, nullptr, nullptr);

	ConMsg("Upload response: %s\n", res.c_str());
}
#else
static void UploadDump(const std::filesystem::path& dumpPath, const std::filesystem::path& metadataPath)
{
	char tokenBuffer[64] = {};
	PresubmitCrashDump(dumpPath.string().c_str(), tokenBuffer, sizeof(tokenBuffer));

	ConMsg("Uploading minidump %s\n", dumpPath.string().c_str());

	std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> strconverter;
	std::map<std::wstring, std::wstring> params;

	params[L"UserID"] = strconverter.from_bytes(g_UserId).c_str();
	params[L"GameDirectory"] = L"csgo";
	params[L"ExtensionVersion"] = strconverter.from_bytes(g_AcceleratorCS2.GetVersion()) + L" [AcceleratorCS2 Build]";
	params[L"ServerID"] = strconverter.from_bytes(g_serverId).c_str();

	if (tokenBuffer[0] != '\0')
	{
		params[L"PresubmitToken"] = strconverter.from_bytes(tokenBuffer).c_str();
	}

	std::map<std::wstring, std::wstring> files;
	files[L"upload_file_minidump"] = dumpPath.wstring();
	files[L"upload_file_metadata"] = metadataPath.wstring();

	std::wstring res;
	google_breakpad::HTTPUpload::SendMultipartPostRequest(L"http://crash.limetech.org/submit", params, files, nullptr, &res, nullptr);

	ConMsg("Upload response: %s\n", strconverter.to_bytes(res).c_str());
}
#endif

// Runs on a detached thread at load, so nothing here may throw: an exception escaping a
// std::thread calls std::terminate and takes the game server down with it. Every filesystem
// call uses the error_code overload for that reason.
//
// A dump is renamed to *_uploaded.dmp *before* it is uploaded. Renaming only after the
// upload returned meant a hung or failed upload (or a server restart mid-upload) left the
// dump in place, and it was presubmitted again on every boot.
void UploadThread()
{
	try
	{
		std::error_code ec;
		const std::filesystem::path dumpDir(dumpStoragePath);

		if (!std::filesystem::is_directory(dumpDir, ec))
		{
			return;
		}

		// Snapshot first: renaming entries while iterating the same directory is unspecified.
		std::vector<std::filesystem::path> pending;
		for (std::filesystem::directory_iterator it(dumpDir, ec), end; !ec && it != end; it.increment(ec))
		{
			const std::filesystem::path& path = it->path();
			if (path.extension() != ".dmp" || path.stem().string().find("_uploaded") != std::string::npos)
			{
				continue;
			}

			if (it->is_regular_file(ec))
			{
				pending.push_back(path);
			}
		}

		if (ec)
		{
			ConMsg("Accelerator: could not list %s: %s\n", dumpStoragePath, ec.message().c_str());
		}

		for (const auto& dumpPath : pending)
		{
			std::filesystem::path metadataPath = dumpPath;
			metadataPath.replace_extension(".dmp.txt");

			std::filesystem::path uploadedPath = dumpPath;
			uploadedPath.replace_filename(dumpPath.stem().string() + "_uploaded" + dumpPath.extension().string());

			std::filesystem::rename(dumpPath, uploadedPath, ec);
			if (ec)
			{
				ConMsg("Accelerator: skipping %s, could not mark it uploaded: %s\n", dumpPath.string().c_str(), ec.message().c_str());
				ec.clear();
				continue;
			}

			UploadDump(uploadedPath, metadataPath);
		}
	}
	catch (const std::exception& e)
	{
		ConMsg("Accelerator: upload thread failed: %s\n", e.what());
	}
	catch (...)
	{
		ConMsg("Accelerator: upload thread failed with an unknown exception\n");
	}
}

static bool HasStackwalk(const std::filesystem::path& metadataPath)
{
	std::ifstream in(metadataPath, std::ios::binary);
	std::string line;
	while (std::getline(in, line))
	{
		if (line.rfind("Crash reason:", 0) == 0 || line.rfind("No crash", 0) == 0 || line.rfind(kStackwalkFailedMarker, 0) == 0)
			return true;
	}
	return false;
}

// The crash handler walks the stack in the dying process, which fails when the heap is corrupt
// (SIGABRT from glibc). Those .dmp.txt files only have the CONFIG block, so fill them in now.
static void RepairIncompleteMetadata()
{
	std::error_code ec;
	std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pending;
	for (std::filesystem::directory_iterator it(dumpStoragePath, ec), end; !ec && it != end; it.increment(ec))
	{
		const std::filesystem::path& path = it->path();
		if (path.extension() != ".dmp")
			continue;

		// Uploaded dumps are renamed to <id>_uploaded.dmp but keep <id>.dmp.txt.
		std::string id = path.stem().string();
		const std::string uploadedSuffix = "_uploaded";
		if (id.size() > uploadedSuffix.size() && id.compare(id.size() - uploadedSuffix.size(), uploadedSuffix.size(), uploadedSuffix) == 0)
			id.erase(id.size() - uploadedSuffix.size());

		std::filesystem::path metadataPath = path.parent_path() / (id + ".dmp.txt");
		if (!HasStackwalk(metadataPath))
			pending.emplace_back(path, metadataPath);
	}

	if (pending.empty())
		return;

	google_breakpad::MinidumpThreadList::set_max_threads(std::numeric_limits<uint32_t>::max());
	google_breakpad::MinidumpMemoryList::set_max_regions(std::numeric_limits<uint32_t>::max());

	// Breakpad logs every stream it reads to std::clog.
	std::streambuf* savedClog = std::clog.rdbuf(nullptr);

	for (const auto& [dumpPath, metadataPath] : pending)
	{
		FILE* out = fopen(metadataPath.string().c_str(), "ab");
		if (!out)
			continue;

		google_breakpad::BasicSourceLineResolver resolver;
		google_breakpad::MinidumpProcessor processor(nullptr, &resolver);
		google_breakpad::Minidump dump(dumpPath.string());
		google_breakpad::ProcessState state;
		if (dump.Read() && processor.Process(&dump, &state) == google_breakpad::PROCESS_OK)
		{
			fprintf(out, "-------- STACKWALK (recovered on next start) --------\n");
			WriteProcessState(out, state, dump);
			ConMsg("Accelerator: added missing stack walk to %s\n", metadataPath.string().c_str());
		}
		else
		{
			fprintf(out, "%s\n", kStackwalkFailedMarker);
			ConMsg("Accelerator: could not process %s\n", dumpPath.string().c_str());
		}
		fclose(out);
	}

	std::clog.rdbuf(savedClog);
}

// Same no-throw rules as UploadThread: this is a detached thread.
void DumpMaintenanceThread()
{
	try
	{
		RepairIncompleteMetadata();
	}
	catch (const std::exception& e)
	{
		ConMsg("Accelerator: repairing dump metadata failed: %s\n", e.what());
	}
	catch (...)
	{
		ConMsg("Accelerator: repairing dump metadata failed with an unknown exception\n");
	}

	if (g_UploadCrashDumps)
		UploadThread();
}

void LoadRecentDumps()
{
	g_NumRecentDumps = 0;

	std::error_code ec;
	std::filesystem::directory_iterator it(dumpStoragePath, ec);
	if (ec)
		return;

	for (const auto& entry : it)
	{
		// Uploaded dumps are renamed to *_uploaded.dmp, they still count.
		if (entry.path().extension() != ".dmp")
			continue;

		struct stat st;
		if (stat(entry.path().string().c_str(), &st) != 0)
			continue;

		if (g_NumRecentDumps < kMaxRecentDumps)
		{
			g_RecentDumpTimes[g_NumRecentDumps++] = st.st_mtime;
			continue;
		}

		// Full: keep the newest ones by replacing the oldest.
		int oldest = 0;
		for (int i = 1; i < kMaxRecentDumps; ++i)
		{
			if (g_RecentDumpTimes[i] < g_RecentDumpTimes[oldest])
				oldest = i;
		}
		if (st.st_mtime > g_RecentDumpTimes[oldest])
			g_RecentDumpTimes[oldest] = st.st_mtime;
	}
}

void LoadServerId()
{
	std::string serverIdPath = std::string(crashGamePath) + "/addons/AcceleratorCS2/serverid.txt";
	std::ifstream serverIdFile(serverIdPath);
	if (serverIdFile.is_open()) {
		serverIdFile >> g_serverId;
		serverIdFile.close();
	}
	else {
		char buffer[64];
		V_snprintf(buffer, sizeof(buffer), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, 0x40 | ((rand() % 255) & 0x0F), rand() % 255,
			0x80 | ((rand() % 255) & 0x3F), rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255);
		g_serverId = buffer;

		std::ofstream serverIdFile(serverIdPath);
		if (serverIdFile.is_open()) {
			serverIdFile << g_serverId;
			serverIdFile.close();
		}
	}
};

void LoadConfig()
{
	// load json config
	std::string configPath = std::string(crashGamePath) + "/addons/AcceleratorCS2/config.json";
	std::ifstream configFile(configPath);
	if (configFile.is_open()) {
		// Parse without exceptions: this runs inside Load(), and a throw here would abort the server.
		nlohmann::json config = nlohmann::json::parse(configFile, nullptr, false);
		configFile.close();

		if (config.is_discarded() || !config.is_object()) {
			ConMsg("Accelerator: %s is not valid JSON, using defaults\n", configPath.c_str());
			return;
		}

		if (config.contains("MinidumpAccountSteamId64") && config["MinidumpAccountSteamId64"].is_string()) {
			g_UserId = config["MinidumpAccountSteamId64"].get<std::string>();
		}

		// true: crashes that happen after the server was told to stop (quit, SIGTERM, restart from a panel)
		// are not dumped, since those are teardown noise rather than real crashes.
		if (config.contains("IgnoreShutdownCrashes") && config["IgnoreShutdownCrashes"].is_boolean()) {
			g_IgnoreShutdownCrashes = config["IgnoreShutdownCrashes"].get<bool>();
		}

		if (config.contains("CrashLoopMaxDumps") && config["CrashLoopMaxDumps"].is_number_integer()) {
			g_CrashLoopMaxDumps = std::clamp(config["CrashLoopMaxDumps"].get<int>(), 0, kMaxRecentDumps);
		}

		if (config.contains("CrashLoopWindowMinutes") && config["CrashLoopWindowMinutes"].is_number_integer()) {
			g_CrashLoopWindowMinutes = std::max(config["CrashLoopWindowMinutes"].get<int>(), 0);
		}

		// false: crashes are still written to the dumps folder, but nothing is presubmitted or
		// uploaded at startup.
		if (config.contains("UploadCrashDumps") && config["UploadCrashDumps"].is_boolean()) {
			g_UploadCrashDumps = config["UploadCrashDumps"].get<bool>();
		}
	}
	else {
		nlohmann::json config;
		config["MinidumpAccountSteamId64"] = "";
		config["UploadCrashDumps"] = false;
		config["IgnoreShutdownCrashes"] = true;
		config["CrashLoopMaxDumps"] = 5;
		config["CrashLoopWindowMinutes"] = 10;

		std::ofstream configFile(configPath);
		if (configFile.is_open()) {
			configFile << config.dump(2);
			configFile.close();
		}
	}
}

bool AcceleratorCS2::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	strncpy(crashGamePath, ismm->GetBaseDir(), sizeof(crashGamePath) - 1);
	ismm->Format(dumpStoragePath, sizeof(dumpStoragePath), "%s/addons/AcceleratorCS2/dumps", ismm->GetBaseDir());

	std::error_code dumpDirError;
	std::filesystem::create_directories(dumpStoragePath, dumpDirError);
	if (dumpDirError)
	{
		ConMsg("Accelerator: could not create %s: %s\n", dumpStoragePath, dumpDirError.message().c_str());
	}

#if defined _LINUX
	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	exceptionHandler = new google_breakpad::ExceptionHandler(descriptor, filterCallback, dumpCallback, NULL, true, -1);

	struct sigaction oact;
	sigaction(SIGSEGV, NULL, &oact);
	SignalHandler = oact.sa_sigaction;

	gameFrameHook.Add(g_pSource2Server);
	InstallShutdownSignalHandlers();
#else
	wchar_t* buf = new wchar_t[sizeof(dumpStoragePath)];
	size_t num_chars = mbstowcs(buf, dumpStoragePath, sizeof(dumpStoragePath));

	exceptionHandler = new google_breakpad::ExceptionHandler(
		std::wstring(buf, num_chars), filterCallback, dumpCallback, NULL, google_breakpad::ExceptionHandler::HANDLER_ALL,
		static_cast<MINIDUMP_TYPE>(MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo), static_cast<const wchar_t*>(NULL), NULL);

	vectoredHandler = AddVectoredExceptionHandler(0, BreakpadVectoredHandler);
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

	delete buf;
#endif

	startupServerHook.Add(g_pNetworkServerService);
	preShutdownHook.Add(g_pSource2Server);
	dispatchConCommandHook.Add(g_pCVar);

	strncpy(crashCommandLine, CommandLine()->GetCmdLine(), sizeof(crashCommandLine) - 1);

	if (late)
		StartupServer(nullptr, {}, nullptr, nullptr);

	g_LoadTime = time(nullptr);

	LoadServerId();
	LoadConfig();
	LoadRecentDumps();
	BuildPluginList();

	if (IsCrashLooping())
	{
		ConMsg("Accelerator: crash loop detected, %d dumps in the last %d minutes (CrashLoopMaxDumps=%d), new crashes will not be dumped until older ones age out\n",
			CountRecentDumps(), g_CrashLoopWindowMinutes, g_CrashLoopMaxDumps);
	}

	if (g_UploadCrashDumps)
		ConMsg("Start accelerator uploader thread\n");
	else
		ConMsg("Accelerator: crash dump upload disabled (UploadCrashDumps=false), dumps are kept in %s\n", dumpStoragePath);

	// Stack walks are repaired before uploading so the uploaded metadata is complete.
	std::thread(DumpMaintenanceThread).detach();

	return true;
}

bool AcceleratorCS2::Unload(char* error, size_t maxlen)
{
#if defined _LINUX
	gameFrameHook.Remove(g_pSource2Server);
	RemoveShutdownSignalHandlers();
#else
	SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#endif
	startupServerHook.Remove(g_pNetworkServerService);
	preShutdownHook.Remove(g_pSource2Server);
	dispatchConCommandHook.Remove(g_pCVar);

	delete exceptionHandler;

	return true;
}

#if defined _LINUX

KHook::Return<void> AcceleratorCS2::GameFrame(IServerGameDLL* pThis, bool simulating, bool bFirstTick, bool bLastTick)
{
	bool weHaveBeenFuckedOver = false;
	struct sigaction oact;

	for (int i = 0; i < kNumHandledSignals; ++i)
	{
		sigaction(kExceptionSignals[i], NULL, &oact);

		if (oact.sa_sigaction != SignalHandler)
		{
			weHaveBeenFuckedOver = true;
			break;
		}
	}

	if (!weHaveBeenFuckedOver)
		return {KHook::Action::Ignore};

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);

	for (int i = 0; i < kNumHandledSignals; ++i)
		sigaddset(&act.sa_mask, kExceptionSignals[i]);

	act.sa_sigaction = SignalHandler;
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;

	for (int i = 0; i < kNumHandledSignals; ++i)
		sigaction(kExceptionSignals[i], &act, NULL);

	return {KHook::Action::Ignore};
}

#endif

KHook::Return<void> AcceleratorCS2::StartupServer(INetworkServerService* pThis, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*)
{
	strncpy(crashMap, g_pNetworkServerService->GetIGameServer()->GetMapName(), sizeof(crashMap) - 1);

	// Plugins can be added or hot-reloaded between maps.
	BuildPluginList();

	return {KHook::Action::Ignore};
}

KHook::Return<void> AcceleratorCS2::PreShutdown(IServerGameDLL* pThis)
{
	g_ShuttingDown = 1;

	return {KHook::Action::Ignore};
}

KHook::Return<void> AcceleratorCS2::DispatchConCommand(ICvar* pThis, ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args)
{
	// Pterodactyl's stop button and most panels send "quit" over the console; catch it before teardown starts.
	const char* name = args.ArgC() > 0 ? args.Arg(0) : nullptr;
	if (name && (!V_stricmp(name, "quit") || !V_stricmp(name, "exit") || !V_stricmp(name, "_restart")))
		g_ShuttingDown = 1;

	// e.g. mp_restartgame or a plugin command right before a crash.
	if (name && name[0])
		RecordCommand(name, args);

	return {KHook::Action::Ignore};
}

const char* AcceleratorCS2::GetLicense()
{
	return "GPLv3";
}

const char* AcceleratorCS2::GetVersion()
{
	return "3.1";
}

const char* AcceleratorCS2::GetDate()
{
	return __DATE__;
}

const char* AcceleratorCS2::GetLogTag()
{
	return "AcceleratorCS2";
}

const char* AcceleratorCS2::GetAuthor()
{
	return "Poggu, Phoenix (˙·٠●Феникс●٠·˙), asherkin";
}

const char* AcceleratorCS2::GetDescription()
{
	return "Crash Handler";
}

const char* AcceleratorCS2::GetName()
{
	return "AcceleratorCS2";
}

const char* AcceleratorCS2::GetURL()
{
	return "https://github.com/mrc4tt/AcceleratorCS2";
}