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

// What the analyzer concluded about one report. Both the text summary and the JSON file are made from it.
struct Summary
{
	std::string verdict;      // "Likely cause: plugin \"X\"" / "Not caused by a plugin" / "Cause: unknown"
	std::string cause;        // "plugin", "not_plugin", "module" (a native module, e.g. a Metamod plugin) or "unknown"
	std::string plugin;       // CounterStrikeSharp plugin blamed, empty if none
	std::string module;       // native module the crash happened in, empty if none
	std::string confidence;   // High / Medium / Low
	std::string title;
	std::string explanation;
	std::vector<std::string> advice;
	std::vector<std::string> evidence;
	std::string signature;    // equal signatures mean the same cause
	std::string pluginInfo;   // version/path of the blamed plugin, if the report lists it
	long uptime = -1;         // seconds, -1 if unknown
	bool cssRecorded = false; // the CounterStrikeSharp crash recorder supplied activity
	bool failed = false;      // the analysis threw; only verdict is set
};

// Analyzes a report without a summary. Never throws: on failure, Summary::failed is set.
Summary Analyze(const std::string& report, const std::vector<Rule>& rules);

// Returns the summary block (kSummaryBegin ... kSummaryEnd, newline-terminated). report is the full
// .dmp.txt. Never throws.
std::string BuildSummary(const std::string& report, const std::vector<Rule>& rules);

// The same analysis as JSON, for tools that read the dumps folder (e.g. a hosting panel) and should not
// parse the text. A summary already at the top of report is skipped. Never throws.
std::string BuildSummaryJson(const std::string& report, const std::vector<Rule>& rules);

std::string FormatSummary(const Summary& summary);
std::string SummaryToJson(const Summary& summary);

// report without the summary block at its top (unchanged if it has none).
std::string StripSummary(const std::string& report);

// True if report already starts with a summary.
bool HasSummary(const std::string& report);

// The "Signature:" value of a summary or report, empty if none. Equal signatures mean the same cause.
std::string GetSignature(const std::string& report);

} // namespace crash_analysis
