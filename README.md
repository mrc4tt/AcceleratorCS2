# AcceleratorCS2

A fork of [AcceleratorLocal](https://github.com/komashchenko/AcceleratorLocal) which is a fork of [asherkin's accelerator](https://github.com/asherkin/accelerator) using Premake with Linux and Windows support.

## Configuration

The configuration file is located at `AcceleratorCS2/config.json`.

| Key | Default | Description |
|-----|---------|-------------|
| `MinidumpAccountSteamId64` | `""` | Your steamid64. This allows you to view full details for your crash dumps on [Throttle](https://crash.limetech.org/). |
| `UploadCrashDumps` | `false` | Upload dumps to Throttle at startup. When `false`, dumps are only kept in `addons/AcceleratorCS2/dumps`. |
| `IgnoreShutdownCrashes` | `true` | Don't write a dump for crashes that happen after the server was told to stop (`quit`, `SIGTERM` from `docker stop`/Pterodactyl, Ctrl+C). These are teardown crashes, not real ones. |
| `IgnoreFatalErrors` | `true` | Don't write a minidump when the engine stops itself on purpose after a fatal error (e.g. `Error reading from loaded packed store` from a broken workshop VPK, or `Failed to update networkable loadout` from a spoofed SteamID). The engine still exits, since it can't continue after a fatal error. A `fatal-<unixtime>.txt` report with the context and a crash summary is written instead. |
| `CrashLoopMaxDumps` | `5` | Stop writing new dumps once this many were written within `CrashLoopWindowMinutes`, so a crash-looping server doesn't fill the disk. `0` disables. |
| `CrashLoopWindowMinutes` | `10` | Window used by `CrashLoopMaxDumps`. |

## Crash summary

Every report starts with a `CRASH SUMMARY` block written for server owners: the likely cause (a plugin, a native module, or not a plugin at all), how sure it is, what happened in plain words, the evidence, and what to do. It is also printed to the console when the crash happens, and once more on the next server start together with how many reports in the dumps folder have the same cause.

```
================ CRASH SUMMARY ================
Verdict:    Likely cause: plugin "IdentitySpoofer"
Confidence: High
What:       Player loadout could not be updated (SteamID / inventory mismatch)
...
Evidence:
  Game error: OnLoadoutChanged(): Failed to update networkable loadout
  [+1040s, 22s before crash] IdentitySpoofer steamid: slot 3 m_steamID changed to 7656..., authenticated SteamID is 7656...
  [+1040s, 22s before crash] IdentitySpoofer command: CSs_spoof @me 7656...
...
Signature:  rule:loadout-steamid-mismatch
================================================
```

Plugins are only named with a CounterStrikeSharp build that has the crash recorder (`CSSCrashContext001`, see `css_crash_context.h`). It records which plugin callback is running, recent plugin activity (commands, events, timers, hooks, load/unload), exceptions plugins threw, and risky state changes such as a player's `m_steamID` no longer matching their authenticated SteamID. Without it the summary still explains engine errors, crashes in native modules, and shows the last console commands.

Reports written by older versions get a summary on the next start.

### Custom rules

`addons/AcceleratorCS2/crash_rules.json` (optional) adds rules that are tried before the built-in ones. It is a JSON array:

```json
[
  {
    "id": "my-plugin-null-pawn",
    "engineError": "",
    "module": "MyNativePlugin.so",
    "crashReason": "",
    "journalKind": "",
    "exception": "",
    "title": "MyNativePlugin crashed on a missing pawn",
    "explanation": "Known bug in MyNativePlugin 1.2 when a player disconnects mid-round.",
    "advice": ["Update MyNativePlugin to 1.3."],
    "suspectFrom": ["current", "activity"],
    "pluginCause": true
  }
]
```

Every non-empty condition must match (case-insensitive substring): `engineError` (engine error/warning logged before the crash), `module` (one of the top trusted frames), `crashReason` (e.g. `SIGABRT`), `journalKind` (a CounterStrikeSharp journal entry, e.g. `steamid`), `exception` (a recorded plugin exception). `suspectFrom` says where to find the plugin to blame, in order: `journal:<kind>`, `current` (running callback), `exception`, `activity` (last active plugin). `pluginCause: false` is for causes that aren't plugins (e.g. a broken map file).

## Dump metadata

Each dump gets a `<id>.dmp.txt` next to it:

- `CONFIG` block: map, game path, command line.
- `CONTEXT` block: uptime, installed CounterStrikeSharp plugins (these load from memory and never show up in the module list), the last 16 console commands, the last 24 engine warnings/errors, the engine fatal error if there was one, and the CounterStrikeSharp crash recorder sections (running callbacks, recent plugin activity, journal, plugin exceptions). Arguments of commands whose name contains `pass`, `rcon`, `token`, `key`, `secret`, `auth`, `webhook` or `discord`, and webhook URLs (Discord, Slack, Telegram) passed to any command, are shown as `*****`. The same values are masked in the `CommandLine`. Client commands such as `say` are recorded too.
- Stack walk, plus the bytes at the crashing instruction. Compiler-generated null-dereference traps (`mov rax,[0]; ud2`) are called out. These blocks often sit just before the entry point of the function that jumped to them, so address-to-function tools blame the wrong function.

The stack walk runs inside the crashing process. After heap corruption (glibc `double free or corruption` → SIGABRT) this can fail and leave only the `CONFIG`/`CONTEXT` blocks. The missing stack walk is then added on the next server start, before any upload.

## Linux builds

| Package | Built in | Runs on |
|---------|----------|---------|
| `steamrt3` | Steam Runtime 3 (sniper, glibc 2.31) | The default CS2 runtime, or directly on Debian 11/12/13, Ubuntu 20.04+ |
| `steamrt4` | Steam Runtime 4 (glibc 2.41) | Servers running under Steam Runtime 4, or directly on hosts with glibc 2.38+ (Debian 13, Ubuntu 24.04+) |

If unsure, use `steamrt3`.

## Building

Linux packages are built inside Docker images based on Valve's official Steam Runtime SDK images (`docker/steamrt3.Dockerfile`, `docker/steamrt4.Dockerfile`). CI and local builds use the same script (needs Docker):

```sh
scripts/build-linux.sh            # both runtimes -> dist/steamrt3, dist/steamrt4
scripts/build-linux.sh steamrt4   # just one
```

`HL2SDKCS2` and `MMSOURCE_DEV` are used if set, otherwise hl2sdk (`cs2`) and metamod-source (`master`) are cloned into `.deps/`. Metamod must be new enough to have `METAMOD_PLAPI_VERSION` 18 (KHook), older headers produce a plugin current Metamod refuses to load.

### CI

`.github/workflows/build.yml` runs Windows, SteamRT3 and SteamRT4 as separate jobs (the Linux ones through `.github/workflows/linux-build.yml`). A manual run (Actions → CI → Run workflow) can build just one of them. Releases are automatic: on a push to `master`, if the version returned by `AcceleratorCS2::GetVersion()` in `extension.cpp` has no `v<version>` release yet, the tag and a GitHub release with all three packages are created. Bump `GetVersion()` to publish a new release; other pushes don't release anything.

### Self-hosted CI runner

The runner machine needs Docker, and the runner's user must be in the `docker` group. `scripts/setup-runner.sh` registers the current Linux machine as a self-hosted runner for the repository, installs it as a service and sets the `SELF_HOSTED_LINUX` repository variable so the Linux jobs use it. Pull requests always build on GitHub-hosted runners, so code from forks never runs on your machine. Set `SELF_HOSTED_LINUX` to `false` to switch back.

On a server without `gh`, pass a registration token (Settings → Actions → Runners → New self-hosted runner, valid 1 hour) and set the variable in the repository settings yourself:

```sh
RUNNER_TOKEN=<token> scripts/setup-runner.sh
```

## Possible conflicts with CounterStrikeSharp

### Windows startup crash

**You must run the server with `-DoNotPreloadDLLs` startup parameter if running Accelerator with CS# on Windows.** The engine loops over every single loaded dll and tries to read an address in every single page of memory. This causes access violations for the .NET binaries that Accelerator catches and handles (dumps and quits).

### Uncaught C# exceptions crashing

If any C# plugin code throws an exception that isn't caught (e.g. no `try`/`catch` handling), Accelerator will catch and handle it (dumps and quits). This is generally easy to spot, because the top function in the stack trace will be `memfd:doublemapper (deleted)`. **Running Accelerator with CS# plugins that do not take this into account will introduce new "crashes" that would not happen otherwise.**