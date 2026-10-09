#pragma once

// Turns a finished .dmp.txt report into a plain-language CRASH SUMMARY for server owners: what happened,
// which plugin most likely caused it, the evidence, and what to do. Pure text in, text out (no SDK, no
// breakpad), so it runs in the crash handler after the stack walk, on the next start for reports that
// are missing a summary, and in tests against real reports.

#include <string>
#include <vector>

namespace crash_analysis {

// Section headers written into the CONTEXT block by the crash handler and read back by the analyzer.
inline constexpr const char* kEngineMessagesHeader = "Engine errors and warnings (oldest first):";
inline constexpr const char* kEngineFatalPrefix = "Engine fatal error: ";
inline constexpr const char* kCssCurrentHeader = "CounterStrikeSharp running callbacks (outermost first):";
inline constexpr const char* kCssActivityHeader = "CounterStrikeSharp recent plugin activity (oldest first):";
inline constexpr const char* kCssJournalHeader = "CounterStrikeSharp journal (oldest first):";
inline constexpr const char* kCssExceptionsHeader = "CounterStrikeSharp plugin exceptions (oldest first):";

inline constexpr const char* kSummaryBegin = "================ CRASH SUMMARY ================";
inline constexpr const char* kSummaryEnd = "================================================";
inline constexpr const char* kSignaturePrefix = "Signature:  ";

struct Rule
{
	std::string id;
	// Every non-empty condition must match (case-insensitive substring).
	std::string engineError;  // an engine error/warning line, or the engine fatal error
	std::string module;       // module of one of the top trusted frames of the crashed thread
	std::string crashReason;  // e.g. "SIGABRT"
	std::string journalKind;  // a CounterStrikeSharp journal entry of this kind exists
	std::string exception;    // a recorded plugin exception contains this text

	std::string title;
	std::string explanation;
	std::vector<std::string> advice;
	// Where to find the plugin to blame, tried in order: "journal:<kind>", "current", "exception", "activity".
	std::vector<std::string> suspectFrom;
	// false for causes that are not a plugin (e.g. a broken map file): no plugin is blamed.
	bool pluginCause = true;
};

// Built-in rules followed by the ones in rulesJson (an array of rule objects, see crash_rules.json in the
// package). Rules from the file are tried first. Invalid JSON is ignored and reported in *error.
std::vector<Rule> LoadRules(const std::string& rulesJson, std::string* error = nullptr);

// Returns the summary block (kSummaryBegin ... kSummaryEnd, newline-terminated). report is the full
// .dmp.txt. Never throws.
std::string BuildSummary(const std::string& report, const std::vector<Rule>& rules);

// True if report already starts with a summary.
bool HasSummary(const std::string& report);

// The "Signature:" value of a summary or report, empty if none. Equal signatures mean the same cause.
std::string GetSignature(const std::string& report);

} // namespace crash_analysis
