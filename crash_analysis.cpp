#include "crash_analysis.h"

#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace crash_analysis {

namespace {

// Built-in rules. The same format is accepted in addons/AcceleratorCS2/crash_rules.json.
const char* const kBuiltinRules = R"json([
  {
    "id": "loadout-steamid-mismatch",
    "engineError": "Failed to update networkable loadout",
    "title": "Player loadout could not be updated (SteamID / inventory mismatch)",
    "explanation": "The game stopped the server on purpose because a player's loadout (inventory items) could not be updated. This happens when the SteamID on a player's controller (m_steamID) no longer matches the Steam account the player actually logged in with, which is what SteamID/identity spoofing plugins do. Skin and inventory plugins that change a player's items can cause it too.",
    "advice": [
      "Disable the plugin named in the verdict, or stop using its SteamID/identity spoofing feature.",
      "If it keeps happening without that plugin, update or disable skin/inventory plugins (e.g. WeaponPaints)."
    ],
    "suspectFrom": ["journal:steamid", "current", "activity"]
  },
  {
    "id": "broken-vpk",
    "engineError": "Error reading from loaded packed store",
    "title": "A map or workshop file on disk is broken",
    "explanation": "The game could not read one of its packed (.vpk) files, usually a workshop map that was only partly downloaded or got corrupted.",
    "advice": [
      "Delete the workshop map's folder so it is downloaded again, or verify the game files.",
      "Switch to another map to confirm the server itself is fine."
    ],
    "pluginCause": false
  }
])json";

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
	return needle.empty() || Lower(haystack).find(Lower(needle)) != std::string::npos;
}

bool StartsWith(const std::string& s, const char* prefix)
{
	return s.rfind(prefix, 0) == 0;
}

std::string Trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	size_t e = s.find_last_not_of(" \t\r\n");
	return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::string line;
	std::istringstream in(text);
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

// "[+1040s] rest" -> 1040, "rest". Lines without a time get -1.
long ParseUptime(const std::string& line, std::string* rest)
{
	std::string t = Trim(line);
	if (StartsWith(t, "[+"))
	{
		size_t close = t.find("s]");
		if (close != std::string::npos)
		{
			long value = std::strtol(t.c_str() + 2, nullptr, 10);
			*rest = Trim(t.substr(close + 2));
			return value;
		}
	}
	*rest = t;
	return -1;
}

struct CssEntry
{
	long uptime = -1;
	std::string plugin;
	std::string kind;
	std::string detail;
};

// "[+1040s] IdentitySpoofer command: CSs_spoof @me 7656..." (the time is absent for running callbacks).
CssEntry ParseCssEntry(const std::string& line)
{
	CssEntry entry;
	std::string rest;
	entry.uptime = ParseUptime(line, &rest);

	size_t space = rest.find(' ');
	size_t colon = rest.find(": ", space == std::string::npos ? 0 : space);
	if (space == std::string::npos || colon == std::string::npos)
	{
		entry.detail = rest;
		return entry;
	}
	entry.plugin = rest.substr(0, space);
	entry.kind = Trim(rest.substr(space + 1, colon - space - 1));
	entry.detail = rest.substr(colon + 2);
	return entry;
}

struct Frame
{
	std::string module;
	std::string offset;
	bool trusted = false;
};

struct Report
{
	long uptime = -1;
	std::vector<std::string> plugins;
	std::vector<std::string> commands;
	std::vector<std::pair<long, std::string>> engineMessages;
	std::string engineFatal;
	std::vector<CssEntry> cssCurrent;
	std::vector<CssEntry> cssActivity;
	std::vector<CssEntry> cssJournal;
	std::vector<CssEntry> cssExceptions;
	std::string crashReason;
	std::string ipBytes;
	std::vector<Frame> frames;
	bool isFatalReport = false;
};

bool IsTrustDescription(const std::string& s)
{
	return ContainsNoCase(s, "instruction pointer in context") || ContainsNoCase(s, "call frame info") || ContainsNoCase(s, "frame pointer");
}

Report Parse(const std::string& text)
{
	Report report;
	enum class Section { None, Plugins, Commands, Engine, CssCurrent, CssActivity, CssJournal, CssExceptions } section = Section::None;
	bool inContext = false;
	bool inCrashedThread = false;

	for (const std::string& line : SplitLines(text))
	{
		if (line == "-------- CONTEXT BEGIN --------") { inContext = true; continue; }
		if (line == "-------- CONTEXT END --------") { inContext = false; section = Section::None; continue; }

		if (inContext)
		{
			if (StartsWith(line, "Uptime=")) { report.uptime = std::strtol(line.c_str() + 7, nullptr, 10); section = Section::None; continue; }
			if (line == "Plugins:") { section = Section::Plugins; continue; }
			if (StartsWith(line, "Recent console commands")) { section = Section::Commands; continue; }
			if (line == kEngineMessagesHeader) { section = Section::Engine; continue; }
			if (line == kCssCurrentHeader) { section = Section::CssCurrent; continue; }
			if (line == kCssActivityHeader) { section = Section::CssActivity; continue; }
			if (line == kCssJournalHeader) { section = Section::CssJournal; continue; }
			if (line == kCssExceptionsHeader) { section = Section::CssExceptions; continue; }
			if (StartsWith(line, kEngineFatalPrefix)) { report.engineFatal = Trim(line.substr(strlen(kEngineFatalPrefix))); section = Section::None; continue; }
			if (!StartsWith(line, "  ")) { section = Section::None; continue; }

			std::string rest;
			switch (section)
			{
			case Section::Plugins: report.plugins.push_back(Trim(line)); break;
			case Section::Commands: report.commands.push_back(Trim(line)); break;
			case Section::Engine: { long t = ParseUptime(line, &rest); report.engineMessages.emplace_back(t, rest); break; }
			case Section::CssCurrent: report.cssCurrent.push_back(ParseCssEntry(line)); break;
			case Section::CssActivity: report.cssActivity.push_back(ParseCssEntry(line)); break;
			case Section::CssJournal: report.cssJournal.push_back(ParseCssEntry(line)); break;
			case Section::CssExceptions: report.cssExceptions.push_back(ParseCssEntry(line)); break;
			default: break;
			}
			continue;
		}

		if (StartsWith(line, "Crash reason:")) { report.crashReason = Trim(line.substr(13)); continue; }
		if (StartsWith(line, "Crash IP bytes:")) { report.ipBytes = Trim(line.substr(15)); continue; }
		if (StartsWith(line, "-------- FATAL ERROR REPORT")) { report.isFatalReport = true; continue; }

		if (StartsWith(line, "Thread ") && line.find("(crashed)") != std::string::npos) { inCrashedThread = true; continue; }
		if (StartsWith(line, "Thread ") || StartsWith(line, "Loaded modules")) { inCrashedThread = false; continue; }
		if (!inCrashedThread)
			continue;

		// " 0  libtier0.so + 0x209a30" (breakpad), optionally followed by "  (trust)" (our own writer).
		std::string t = Trim(line);
		bool isFrameLine = !t.empty() && std::isdigit(static_cast<unsigned char>(t[0])) && t.find("  ") != std::string::npos;
		if (isFrameLine && t.find(" + 0x") == std::string::npos)
		{
			// " 3  0x7f1234567890": code outside any module, i.e. JIT-compiled .NET code.
			std::string afterIndex = Trim(t.substr(t.find(' ')));
			if (afterIndex.rfind("0x", 0) == 0)
			{
				Frame frame;
				frame.module = afterIndex.substr(0, afterIndex.find(' '));
				size_t paren = afterIndex.find('(');
				if (paren != std::string::npos)
					frame.trusted = IsTrustDescription(afterIndex.substr(paren));
				report.frames.push_back(frame);
			}
			continue;
		}
		if (isFrameLine)
		{
			size_t start = t.find_first_of(' ');
			std::string afterIndex = Trim(t.substr(start));
			size_t plus = afterIndex.find(" + 0x");
			Frame frame;
			frame.module = afterIndex.substr(0, plus);
			size_t offEnd = afterIndex.find_first_of(" (", plus + 3);
			frame.offset = afterIndex.substr(plus + 3, offEnd == std::string::npos ? std::string::npos : offEnd - plus - 3);
			size_t paren = afterIndex.find('(', plus);
			if (paren != std::string::npos)
				frame.trusted = IsTrustDescription(afterIndex.substr(paren));
			report.frames.push_back(frame);
			continue;
		}
		if (StartsWith(t, "Found by:") && !report.frames.empty())
			report.frames.back().trusted = IsTrustDescription(t);
	}
	return report;
}

bool IsValveOrSystemModule(const std::string& module)
{
	static const char* const kKnown[] = {
		"libserver.so", "libengine2.so", "libtier0.so", "libnetworksystem.so", "libvphysics2.so", "libschemasystem.so",
		"libsteamnetworkingsockets.so", "steamclient.so", "libfilesystem_stdio.so", "libresourcesystem.so",
		"libmaterialsystem2.so", "libmeshsystem.so", "libworldrenderer.so", "libanimationsystem.so", "libsoundsystem.so",
		"libscenesystem.so", "libmatchmaking.so", "libparticles.so", "libpulse_system.so", "libvscript.so", "libinputsystem.so",
		"libsteam_api.so", "libv8.so", "liblocalize.so", "libtier1.so", "libvstdlib.so", "cs2",
		"server.dll", "engine2.dll", "tier0.dll", "networksystem.dll", "vphysics2.dll", "schemasystem.dll", "steamclient64.dll",
		"libc.so", "libm.so", "libstdc++", "libgcc_s", "ld-linux", "libpthread", "libdl.so", "librt.so",
		"ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll", "msvcp140.dll", "vcruntime140",
	};
	std::string lower = Lower(module);
	for (const char* known : kKnown)
	{
		if (lower.rfind(known, 0) == 0)
			return true;
	}
	return false;
}

bool IsDotnetModule(const std::string& module)
{
	std::string lower = Lower(module);
	return lower.find("coreclr") != std::string::npos || lower.find("clrjit") != std::string::npos ||
		lower.find("system.private.corelib") != std::string::npos || lower.find("libsystem.native") != std::string::npos;
}

// Native code that is neither Valve's nor the OS's: a Metamod plugin, or Metamod/CounterStrikeSharp itself.
bool IsThirdPartyModule(const std::string& module)
{
	return !module.empty() && module.rfind("0x", 0) != 0 && !IsValveOrSystemModule(module) && !IsDotnetModule(module);
}

bool IsCorePlugin(const std::string& plugin)
{
	return plugin.empty() || plugin == "core" || plugin == "unknown" || plugin == "CounterStrikeSharp.API";
}

// The engine's own "stop now" after a fatal log: movl $0x0,0x0; ud2.
bool IsDeliberateEngineAbort(const Report& report)
{
	return StartsWith(Lower(report.ipBytes), "c7 04 25 00 00 00 00 00 00 00 00 0f 0b");
}

std::vector<std::string> AllEngineErrors(const Report& report)
{
	std::vector<std::string> errors;
	if (!report.engineFatal.empty())
		errors.push_back(report.engineFatal);
	for (const auto& [time, message] : report.engineMessages)
		errors.push_back(message);
	return errors;
}

std::string LongestEngineError(const Report& report)
{
	if (!report.engineFatal.empty())
		return report.engineFatal;
	for (auto it = report.engineMessages.rbegin(); it != report.engineMessages.rend(); ++it)
	{
		if (StartsWith(it->second, "ERROR"))
			return Trim(it->second.substr(5));
	}
	return std::string();
}

bool RuleMatches(const Rule& rule, const Report& report)
{
	if (rule.engineError.empty() && rule.module.empty() && rule.crashReason.empty() && rule.journalKind.empty() && rule.exception.empty())
		return false;

	if (!rule.engineError.empty())
	{
		auto errors = AllEngineErrors(report);
		if (std::none_of(errors.begin(), errors.end(), [&](const std::string& e) { return ContainsNoCase(e, rule.engineError); }))
			return false;
	}
	if (!rule.module.empty())
	{
		bool found = false;
		for (size_t i = 0; i < report.frames.size() && i < 8 && !found; ++i)
			found = report.frames[i].trusted && ContainsNoCase(report.frames[i].module, rule.module);
		if (!found)
			return false;
	}
	if (!rule.crashReason.empty() && !ContainsNoCase(report.crashReason, rule.crashReason))
		return false;
	if (!rule.journalKind.empty() &&
		std::none_of(report.cssJournal.begin(), report.cssJournal.end(), [&](const CssEntry& e) { return e.kind == rule.journalKind; }))
		return false;
	if (!rule.exception.empty() &&
		std::none_of(report.cssExceptions.begin(), report.cssExceptions.end(), [&](const CssEntry& e) { return ContainsNoCase(e.detail, rule.exception); }))
		return false;
	return true;
}

struct Suspect
{
	std::string plugin;
	std::string how;           // short reason, shown as evidence
	const CssEntry* entry = nullptr;
	bool strong = false;       // direct evidence (journal entry, running callback), not just "was active recently"
};

const CssEntry* LastNonCore(const std::vector<CssEntry>& entries, const std::string& kind = std::string())
{
	for (auto it = entries.rbegin(); it != entries.rend(); ++it)
	{
		if (!IsCorePlugin(it->plugin) && (kind.empty() || it->kind == kind))
			return &*it;
	}
	return nullptr;
}

const CssEntry* InnermostNonCore(const std::vector<CssEntry>& current)
{
	return LastNonCore(current);
}

Suspect FindSuspect(const std::vector<std::string>& from, const Report& report)
{
	for (const std::string& source : from)
	{
		if (StartsWith(source, "journal:"))
		{
			if (const CssEntry* e = LastNonCore(report.cssJournal, source.substr(8)))
				return { e->plugin, "recorded by CounterStrikeSharp", e, true };
		}
		else if (source == "current")
		{
			if (const CssEntry* e = InnermostNonCore(report.cssCurrent))
				return { e->plugin, "was running when the server crashed", e, true };
		}
		else if (source == "exception")
		{
			if (const CssEntry* e = LastNonCore(report.cssExceptions))
				return { e->plugin, "threw an error shortly before the crash", e, false };
		}
		else if (source == "activity")
		{
			if (const CssEntry* e = LastNonCore(report.cssActivity))
				return { e->plugin, "was the last plugin active before the crash", e, false };
		}
	}
	return {};
}

std::string FormatEntry(const CssEntry& e, long crashUptime)
{
	std::string s;
	if (e.uptime >= 0)
	{
		s += "[+" + std::to_string(e.uptime) + "s";
		if (crashUptime >= e.uptime)
			s += ", " + std::to_string(crashUptime - e.uptime) + "s before crash";
		s += "] ";
	}
	s += e.plugin + " " + e.kind + ": " + e.detail;
	return s;
}

// "css/IdentitySpoofer (100864 bytes, 2026-10-08 12:44 UTC)" for plugin "IdentitySpoofer".
std::string PluginInfo(const Report& report, const std::string& plugin)
{
	for (const std::string& line : report.plugins)
	{
		if (line.rfind("css/" + plugin + " ", 0) == 0)
			return line;
	}
	return std::string();
}

void Wrap(std::ostringstream& out, const std::string& text, const char* indent, size_t width = 92)
{
	std::istringstream words(text);
	std::string word, line;
	while (words >> word)
	{
		if (!line.empty() && line.size() + 1 + word.size() > width)
		{
			out << indent << line << "\n";
			line.clear();
		}
		line += (line.empty() ? "" : " ") + word;
	}
	if (!line.empty())
		out << indent << line << "\n";
}

} // namespace

std::vector<Rule> LoadRules(const std::string& rulesJson, std::string* error)
{
	std::vector<Rule> rules;
	auto parse = [&rules](const std::string& text, std::string* parseError) {
		nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
		if (json.is_discarded() || !json.is_array())
		{
			if (parseError)
				*parseError = "crash rules must be a JSON array of rule objects";
			return;
		}
		for (const auto& item : json)
		{
			if (!item.is_object())
				continue;
			auto str = [&item](const char* key) { return item.contains(key) && item[key].is_string() ? item[key].get<std::string>() : std::string(); };
			auto list = [&item](const char* key) {
				std::vector<std::string> values;
				if (item.contains(key) && item[key].is_array())
					for (const auto& v : item[key])
						if (v.is_string())
							values.push_back(v.get<std::string>());
				return values;
			};
			Rule rule;
			rule.id = str("id");
			rule.engineError = str("engineError");
			rule.module = str("module");
			rule.crashReason = str("crashReason");
			rule.journalKind = str("journalKind");
			rule.exception = str("exception");
			rule.title = str("title");
			rule.explanation = str("explanation");
			rule.advice = list("advice");
			rule.suspectFrom = list("suspectFrom");
			if (item.contains("pluginCause") && item["pluginCause"].is_boolean())
				rule.pluginCause = item["pluginCause"].get<bool>();
			if (!rule.id.empty() && !rule.title.empty())
				rules.push_back(std::move(rule));
		}
	};

	if (!rulesJson.empty())
		parse(rulesJson, error);
	parse(kBuiltinRules, nullptr);
	return rules;
}

bool HasSummary(const std::string& report)
{
	return report.rfind(kSummaryBegin, 0) == 0;
}

std::string GetSignature(const std::string& report)
{
	size_t pos = report.find(std::string("\n") + kSignaturePrefix);
	if (pos == std::string::npos)
		return std::string();
	pos += 1 + strlen(kSignaturePrefix);
	size_t end = report.find('\n', pos);
	return Trim(report.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

std::string BuildSummary(const std::string& text, const std::vector<Rule>& rules)
{
	try
	{
		Report report = Parse(text);

		std::string verdict;       // "Likely cause: plugin X" / "Cause: not a plugin" / "Cause: unknown"
		std::string confidence;    // High / Medium / Low
		std::string title;
		std::string explanation;
		std::vector<std::string> advice;
		std::vector<std::string> evidence;
		std::string signature;
		Suspect suspect;

		const Rule* matched = nullptr;
		for (const Rule& rule : rules)
		{
			if (RuleMatches(rule, report))
			{
				matched = &rule;
				break;
			}
		}

		// Trusted top frames in a third-party native module (a Metamod plugin, or Metamod/CSS itself).
		std::string thirdPartyModule;
		for (size_t i = 0; i < report.frames.size() && i < 6; ++i)
		{
			const Frame& f = report.frames[i];
			if (!f.trusted)
				continue;
			if (IsThirdPartyModule(f.module))
			{
				thirdPartyModule = f.module + " + " + f.offset;
				break;
			}
		}
		bool crashedInDotnet = !report.frames.empty() && (IsDotnetModule(report.frames[0].module) || report.frames[0].module.rfind("0x", 0) == 0);
		const CssEntry* running = InnermostNonCore(report.cssCurrent);
		std::string engineError = LongestEngineError(report);

		if (matched)
		{
			title = matched->title;
			explanation = matched->explanation;
			advice = matched->advice;
			signature = "rule:" + matched->id;
			if (matched->pluginCause)
			{
				suspect = FindSuspect(matched->suspectFrom.empty() ? std::vector<std::string>{ "current", "exception", "activity" } : matched->suspectFrom, report);
				confidence = suspect.plugin.empty() ? "Medium" : (suspect.strong ? "High" : "Medium");
			}
			else
			{
				verdict = "Not caused by a plugin";
				confidence = "High";
			}
		}
		else if (running)
		{
			suspect = { running->plugin, "was running when the server crashed", running, true };
			title = "The server crashed while a plugin was running";
			explanation = "The crash happened inside a CounterStrikeSharp plugin callback (" + running->kind + ": " + running->detail + ").";
			advice = { "Update the plugin named in the verdict, or disable it and check whether the crashes stop.", "Send this file to the plugin's author." };
			confidence = crashedInDotnet || !thirdPartyModule.empty() ? "High" : "Medium";
			signature = "running:" + running->plugin + ":" + running->kind;
		}
		else if (!thirdPartyModule.empty())
		{
			std::string module = thirdPartyModule.substr(0, thirdPartyModule.find(" + "));
			title = "The server crashed inside " + module;
			explanation = "The crashing code belongs to " + module + ", which is not part of the game. " +
				(ContainsNoCase(module, "metamod") || ContainsNoCase(module, "counterstrikesharp")
					? "This is the plugin loader itself, so the real cause can still be a plugin it was running, or an outdated version after a game update."
					: "This is usually a Metamod plugin.");
			advice = { "Update " + module + " (game updates often break native plugins), or remove it and check whether the crashes stop." };
			verdict = "Likely cause: " + module;
			confidence = "High";
			signature = "module:" + module;
		}
		else if (!engineError.empty() && (IsDeliberateEngineAbort(report) || report.isFatalReport))
		{
			title = "The game stopped the server on purpose after an error";
			explanation = "The game logged this error and then shut itself down: \"" + engineError + "\". No rule explains this error yet.";
			suspect = FindSuspect({ "journal:steamid", "exception", "activity" }, report);
			advice = { "Search for the error message above; it usually names the system that failed.", "If a plugin is named in the verdict, try disabling it first." };
			confidence = "Low";
			signature = "engine:" + engineError.substr(0, 80);
		}
		else
		{
			title = "Cause could not be determined automatically";
			explanation = "The crash happened in the game's own code and no plugin was running at that moment.";
			suspect = FindSuspect({ "exception" }, report);
			advice = { "If this repeats, send this file to support so the stack trace can be looked at.",
				"Check whether the crashes started after installing or updating a plugin." };
			confidence = "Low";
			std::string top = report.frames.empty() ? "?" : report.frames[0].module + "+" + report.frames[0].offset;
			signature = "frame:" + top;
		}

		if (verdict.empty())
		{
			if (!suspect.plugin.empty())
				verdict = "Likely cause: plugin \"" + suspect.plugin + "\"";
			else if (matched && matched->pluginCause)
				verdict = "Likely cause: a plugin (could not tell which one)";
			else
				verdict = "Cause: unknown";
		}

		// Evidence, newest last.
		if (!engineError.empty())
			evidence.push_back("Game error: " + engineError);
		if (suspect.entry)
			evidence.push_back(FormatEntry(*suspect.entry, report.uptime) + "   <- " + suspect.how);
		for (const CssEntry& e : report.cssJournal)
		{
			if (&e != suspect.entry)
				evidence.push_back(FormatEntry(e, report.uptime));
		}
		if (running && running != suspect.entry)
			evidence.push_back("Running: " + running->plugin + " " + running->kind + ": " + running->detail);
		for (const CssEntry& e : report.cssExceptions)
		{
			if (&e != suspect.entry && report.uptime >= 0 && e.uptime >= 0 && report.uptime - e.uptime <= 30)
				evidence.push_back("Error: " + FormatEntry(e, report.uptime));
		}
		if (!thirdPartyModule.empty())
			evidence.push_back("Crashing code: " + thirdPartyModule);
		if (suspect.plugin.empty() && !report.commands.empty())
		{
			// Nothing to blame: the last console commands at least show what was going on.
			size_t first = report.commands.size() > 3 ? report.commands.size() - 3 : 0;
			for (size_t i = first; i < report.commands.size(); ++i)
				evidence.push_back("Console: " + report.commands[i]);
		}
		if (!suspect.plugin.empty())
		{
			// Console commands right before the crash that belong to the suspect, from the activity history.
			for (const CssEntry& e : report.cssActivity)
			{
				if (&e != suspect.entry && e.plugin == suspect.plugin && e.kind == "command")
					evidence.push_back(FormatEntry(e, report.uptime));
			}
		}

		std::ostringstream out;
		out << kSummaryBegin << "\n";
		out << "Verdict:    " << verdict << "\n";
		out << "Confidence: " << confidence << "\n";
		out << "What:       " << title << "\n";
		if (report.uptime >= 0)
			out << "Uptime:     " << report.uptime / 60 << "m" << report.uptime % 60 << "s\n";
		out << "\nWhat happened:\n";
		Wrap(out, explanation, "  ");
		if (!evidence.empty())
		{
			out << "\nEvidence:\n";
			for (const std::string& e : evidence)
				out << "  " << e << "\n";
		}
		if (!suspect.plugin.empty())
		{
			std::string info = PluginInfo(report, suspect.plugin);
			if (!info.empty())
				out << "\nPlugin:     " << info << "\n";
		}
		if (!advice.empty())
		{
			out << "\nWhat to do:\n";
			for (size_t i = 0; i < advice.size(); ++i)
			{
				std::ostringstream item;
				Wrap(item, advice[i], "     ");
				std::string text = item.str();
				out << "  " << (i + 1) << "." << text.substr(4);
			}
		}
		if (report.cssCurrent.empty() && report.cssActivity.empty() && report.cssJournal.empty())
			out << "\nNote: no CounterStrikeSharp activity was recorded (CounterStrikeSharp without the crash\n      recorder, or not installed), so plugins can only be blamed from the game error.\n";
		out << "\n" << kSignaturePrefix << signature << "\n";
		out << kSummaryEnd << "\n\n";
		return out.str();
	}
	catch (...)
	{
		return std::string(kSummaryBegin) + "\nVerdict:    Cause: unknown (analysis failed)\n" + kSummaryEnd + "\n\n";
	}
}

} // namespace crash_analysis
