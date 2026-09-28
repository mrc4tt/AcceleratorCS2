#!/usr/bin/env bash
# Register this Linux machine as a self-hosted GitHub Actions runner for the repository and
# install it as a systemd service. Needs: docker, sudo, curl.
#
# Usage: scripts/setup-runner.sh [owner/repo] [install-dir]
#
# Registration token: taken from $RUNNER_TOKEN if set, otherwise requested with gh (which then must be
# authenticated with admin rights on the repo). Get one from any machine with:
#   gh api -X POST repos/mrc4tt/AcceleratorCS2/actions/runners/registration-token -q .token
# or GitHub -> Settings -> Actions -> Runners -> New self-hosted runner. Tokens expire after 1 hour.
set -euo pipefail

REPO="${1:-mrc4tt/AcceleratorCS2}"
DIR="${2:-$HOME/actions-runner-${REPO##*/}}"

HAVE_GH=0
command -v gh >/dev/null && gh auth status >/dev/null 2>&1 && HAVE_GH=1
if [ -z "${RUNNER_TOKEN:-}" ] && [ "$HAVE_GH" = 0 ]; then
	echo "error: set RUNNER_TOKEN, or install and log in to gh" >&2
	exit 1
fi
command -v docker >/dev/null || { echo "error: docker is not installed (the Linux jobs run in containers)" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: $(whoami) cannot talk to docker, add it to the docker group and log in again" >&2; exit 1; }

# Latest runner release, resolved from the redirect so gh isn't needed.
VERSION="$(curl -fsSLI -o /dev/null -w '%{url_effective}' https://github.com/actions/runner/releases/latest)"
VERSION="${VERSION##*/v}"
TOKEN="${RUNNER_TOKEN:-$( [ "$HAVE_GH" = 1 ] && gh api -X POST "repos/$REPO/actions/runners/registration-token" -q .token )}"

mkdir -p "$DIR"
cd "$DIR"
if [ ! -x ./config.sh ]; then
	curl -fsSL "https://github.com/actions/runner/releases/download/v$VERSION/actions-runner-linux-x64-$VERSION.tar.gz" | tar xz
fi

./config.sh --unattended --replace \
	--url "https://github.com/$REPO" \
	--token "$TOKEN" \
	--name "$(hostname)-${REPO##*/}" \
	--labels "self-hosted,linux,x64"

sudo ./svc.sh install "$(whoami)"
sudo ./svc.sh start

echo "Runner installed in $DIR for $REPO."

# Tell the workflow to send Linux jobs to this runner.
if [ "$HAVE_GH" = 1 ]; then
	gh variable set SELF_HOSTED_LINUX --body true -R "$REPO"
	echo "SELF_HOSTED_LINUX set to true, Linux jobs now use this runner."
else
	echo "Now set the repository variable SELF_HOSTED_LINUX=true (Settings -> Secrets and variables -> Actions -> Variables),"
	echo "or from a machine with gh: gh variable set SELF_HOSTED_LINUX --body true -R $REPO"
fi
