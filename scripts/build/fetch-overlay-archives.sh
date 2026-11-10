#!/bin/bash
# GeneralsX: Give a build host that has no community-patch archives a way
# to obtain them, so "the release refuses to publish an unjoinable bundle" is a gate with a route
# through it rather than a dead end.
#
# THE PROBLEM THIS SOLVES. The archives are third-party community content: ~29 MB mandatory and
# ~58 MB optional of .big that is not ours to redistribute from this repository, and `git ls-files`
# confirms not one .big is tracked. A GitHub-hosted runner therefore starts with nothing, which is
# why the macOS CI job runs the packaging smoke test with GX_BUNDLE_ALLOW_NO_OVERLAY=1. That is
# correct for CI and wrong for a release: an overlay-less zip installs a game that plays
# single-player perfectly and is refused by every peer with DATA_DIFFERS.
#
# So the release path needs a source, and the source has to be configuration rather than code -
# this repository is public, and an operator's host does not go in it.
#
# CONFIGURATION, both from the environment, neither with a default:
#
#   GX_OVERLAY_ARCHIVE_URL      a tar (optionally .gz/.xz/.zst) holding the archives at its root
#   GX_OVERLAY_ARCHIVE_SHA256   the expected sha256 of that tar, lowercase hex
#
# THE CHECKSUM IS MANDATORY WHEN THE URL IS SET, and this is not ceremony. Whatever this fetches is
# copied verbatim into a signed release asset that players run. An unverified download would make
# whoever controls that URL - or anyone who can answer for it - an unreviewed contributor to every
# macOS release. A URL with no checksum is a configuration error and is refused, loudly, rather
# than fetched "just this once".
#
# Usage:  fetch-overlay-archives.sh <dest-dir>
# Exit:   0 fetched and verified   2 not configured (caller decides)   1 configured but failed
#
# Exit 2 is separate on purpose: "no archive source is configured" is a legitimate state for a
# smoke-test build and a fatal one for a release, and only the caller knows which it is running.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=scripts/build/lib-overlay.sh
source "${SCRIPT_DIR}/lib-overlay.sh"

DEST="${1:-}"
if [[ -z "${DEST}" ]]; then
    echo "usage: $(basename "$0") <dest-dir>" >&2
    exit 1
fi

URL="${GX_OVERLAY_ARCHIVE_URL:-}"
SUM="${GX_OVERLAY_ARCHIVE_SHA256:-}"

if [[ -z "${URL}" ]]; then
    echo "No archive source configured (GX_OVERLAY_ARCHIVE_URL is unset)."
    exit 2
fi

if [[ -z "${SUM}" ]]; then
    echo "ERROR: GX_OVERLAY_ARCHIVE_URL is set but GX_OVERLAY_ARCHIVE_SHA256 is not." >&2
    echo "  The fetched bytes are copied into a release asset that players run, so an unverified" >&2
    echo "  download would put whoever answers for that URL inside the release. Set both or neither." >&2
    exit 1
fi

# Normalise so a checksum pasted in upper case is not reported as a mismatch.
SUM="$(printf '%s' "${SUM}" | tr '[:upper:]' '[:lower:]')"
if [[ ! "${SUM}" =~ ^[0-9a-f]{64}$ ]]; then
    echo "ERROR: GX_OVERLAY_ARCHIVE_SHA256 is not 64 hex characters: '${SUM}'" >&2
    exit 1
fi

# sha256sum on Linux runners, shasum on macOS. Resolved once, up front, so a host with neither
# fails here with a clear message instead of somewhere inside a pipeline.
if command -v sha256sum > /dev/null 2>&1; then
    _sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum > /dev/null 2>&1; then
    _sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "ERROR: neither sha256sum nor shasum is available - cannot verify the download." >&2
    exit 1
fi

TMPDIR_FETCH="$(mktemp -d)" || exit 1
trap 'rm -rf "${TMPDIR_FETCH}"' EXIT
TARBALL="${TMPDIR_FETCH}/overlay-archives.tar"

echo "Fetching community-patch archives..."
# --fail so an HTTP error page is an error rather than a 400-byte "tar". The URL itself is not
# echoed: it comes from repository configuration, and this log is public on a public repo.
if ! curl --fail --silent --show-error --location --retry 3 --retry-delay 2 \
        --output "${TARBALL}" "${URL}"; then
    echo "ERROR: download failed (see curl's message above)." >&2
    echo "  Check GX_OVERLAY_ARCHIVE_URL in the repository configuration." >&2
    exit 1
fi

GOT="$(_sha256 "${TARBALL}")"
if [[ "${GOT}" != "${SUM}" ]]; then
    echo "ERROR: checksum mismatch on the fetched archive bundle." >&2
    echo "  expected: ${SUM}" >&2
    echo "  actual:   ${GOT}" >&2
    echo "  Refusing to stage unverified bytes into a release asset. If the bundle was updated on" >&2
    echo "  purpose, update GX_OVERLAY_ARCHIVE_SHA256 in the same change that updated it." >&2
    exit 1
fi
echo "  checksum ok (${GOT})"

mkdir -p "${DEST}" || exit 1
# --strip-components=0 and no wildcards: the bundle is expected to hold the archives at its root.
# Anything nested simply will not satisfy gx_overlay_verify below, which is the check that matters.
if ! tar -xf "${TARBALL}" -C "${DEST}"; then
    echo "ERROR: could not extract the archive bundle into ${DEST}." >&2
    exit 1
fi

# The mandatory set is what decides whether a player can join, so it is verified here rather than
# left for the bundler - a fetch that "succeeded" and produced the wrong set should fail at the
# fetch, where the message can name the configuration that is wrong.
if ! gx_overlay_verify "${DEST}"; then
    echo "ERROR: the fetched bundle does not contain the mandatory overlay set." >&2
    echo "  It must hold the archives named in ${GX_OVERLAY_LIST} at its ROOT, not in a subdirectory." >&2
    exit 1
fi

# Optional archives are reported, never fatal here: the bundler decides what an incomplete optional
# set means, and it has both the policy and the escape hatch for it.
if OPTIONAL_LINES="$(gx_overlay_optional)"; then
    while IFS= read -r _line; do
        [[ -n "${_line}" ]] || continue
        _name="$(gx_overlay_optional_name "${_line}")"
        if [[ -f "${DEST}/${_name}" && -s "${DEST}/${_name}" ]]; then
            echo "  optional present: ${_name}"
        else
            echo "  optional ABSENT:  ${_name}"
        fi
    done <<< "${OPTIONAL_LINES}"
else
    echo "ERROR: cannot read ${GX_OVERLAY_OPTIONAL_LIST}." >&2
    exit 1
fi

echo "Community-patch archives staged in ${DEST}"
exit 0
