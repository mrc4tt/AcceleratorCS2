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

The same summary is also written as JSON next to each report (`<id>.summary.json`, `fatal-<unixtime>.summary.json`), for tools that read the dumps folder, such as a hosting panel, so they don't have to parse the text. It has `verdict`, `cause` (`plugin`, `not_plugin`, `module` or `unknown`), `plugin`, `module`, `confidence`, `title`, `explanation`, `advice`, `evidence`, `signature` and `uptime`, and is scrubbed like the report. Reports that already have a text summary get their JSON file on the next start.

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

## Support

Need help, found a bug, or have a feature idea? Join the Discord support server:

[![Discord](https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/zuQN3U2ZEh)

- **Questions and setup help:** ask in the support channels on Discord.
- **Bug reports:** open a [GitHub issue](https://github.com/mrc4tt/AcceleratorCS2/issues) with your server log, plugin version and steps to reproduce. Discord is fine for a quick check first.
- **Feature requests:** post them on Discord or open an [issue](https://github.com/mrc4tt/AcceleratorCS2/issues).

Please do not send support requests by direct message. Asking in the public channels means others can help and find the answer later.

## Building

Linux packages are built inside Docker images based on Valve's official Steam Runtime SDK images (`docker/steamrt3.Dockerfile`, `docker/steamrt4.Dockerfile`). CI and local builds use the same script (needs Docker):

```sh
scripts/build-linux.sh            # both runtimes -> dist/steamrt3, dist/steamrt4
scripts/build-linux.sh steamrt4   # just one
```

`HL2SDKCS2` and `MMSOURCE_DEV` are used if set, otherwise [mrc4tt/hl2sdk](https://github.com/mrc4tt/hl2sdk) (`cs2`) and metamod-source (`master`) are cloned into `.deps/`. Metamod must be new enough to have `METAMOD_PLAPI_VERSION` 18 (KHook), older headers produce a plugin current Metamod refuses to load.

The Windows package is cross-compiled on Linux too (`docker/windows.Dockerfile`): clang targeting `x86_64-pc-windows-msvc` with lld-link, against an MSVC CRT / Windows SDK sysroot made by [xwin](https://github.com/Jake-Shadle/xwin) (`~/.xwin`, or `XWIN_DIR`; created with `xwin --accept-license splat` if missing):

```sh
scripts/build-windows.sh          # -> dist/windows
```

### Releases

Releases are built and published on the build server by [cs2-build-system](https://github.com/mrc4tt/cs2-build-system)'s `build-all.sh`, in the same images as above (`scripts/container-build.sh`), started from [server-ops](https://github.com/mrc4tt/server-ops) (the `release` workflow or the dashboard's Releases tab):

```sh
gh workflow run release -R mrc4tt/server-ops -f only=AcceleratorCS2                   # dry run: version plan + build
gh workflow run release -R mrc4tt/server-ops -f only=AcceleratorCS2 -f dry_run=false  # bump, tag, push, release
gh workflow run release -R mrc4tt/server-ops -f only=AcceleratorCS2 -f version=4.0 -f dry_run=false
```

The version is `AcceleratorCS2::GetVersion()` in `extension.cpp`, tag `v<version>`. With changes since the last release it's bumped (commit "Release vX") before the build; a `GetVersion()` you bumped by hand is released as is. Push your commits first, the server builds what's on `master`.

`scripts/release.sh` does the same from your own machine (build everything, then `gh release create`), as a fallback.

### CI

`.github/workflows/build.yml` only runs manually (Actions → CI → Run workflow), pushes and pull requests no longer build. It runs Windows, SteamRT3 and SteamRT4 as separate jobs (the Linux ones through `.github/workflows/linux-build.yml`), can build just one of them, and uploads the packages as workflow artifacts. It doesn't release anything.

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