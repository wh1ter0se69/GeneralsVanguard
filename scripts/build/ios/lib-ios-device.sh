# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# lib-ios-device.sh - deterministic selection of the iOS install target.
#
# GeneralsX:
#
# Source this, do not execute it:
#     . "$(dirname "$0")/lib-ios-device.sh"
#
# WHY THIS EXISTS. package-ios-zh.sh used to resolve its target like this:
#
#     devicectl list devices | grep -vi unavailable | grep -iE 'available|connected' \
#         | grep -oE '<uuid-regex>' | head -1
#
# which is "whichever available device devicectl happened to print first". devicectl sorts its
# listing by device NAME, so on a Mac with more than one paired device the install target is
# decided by alphabetical order. On this machine it picked the right iPad only because that iPad's
# name happens to sort before the paired iPhone's. Renaming either device, pairing a new one, or a
# change in devicectl's own ordering silently redirects a GeneralsX install onto the phone -
# and nothing in the output says so, because the script prints the UUID it chose and both
# UUIDs look equally plausible.
#
# So: the target is stated (GX_IOS_DEVICE or --device), auto-selection happens only when it is
# unambiguous, and ambiguity is refused rather than guessed. The iPad model filter is a second
# guard behind that, not the mechanism - "it's the only iPad" is a fact about today's desk, and
# the point of this file is to not depend on facts about today's desk.
#
# TWO IDENTIFIERS, NOT INTERCHANGEABLE. The packaging script needs both:
#   * CoreDevice UUID (8-4-4-4-12) - the "Identifier" column of `devicectl list devices`, and
#     what `devicectl device install --device` expects.
#   * hardware UDID (8-16, e.g. 00008122-XXXXXXXXXXXXXXXX) - what `xcodebuild -destination id=`
#     expects, and what a provisioning profile lists under ProvisionedDevices.
# Passing one where the other belongs fails in unhelpful ways, so gx_ios_select_device resolves
# BOTH from a single listing and hands back a matched pair. As INPUT either one is accepted,
# because an operator pasting from their own notes has no reason to know which they wrote down.

# Emit one TSV row per paired device: identifier, udid, deviceType, platform, tunnelState, name.
# Parsed from --json-output, not the text table, whose State text varies by Xcode version and whose names hold spaces.
# Returns non-zero if the listing cannot be read. Never prints diagnostics itself.
gx_ios_device_table() {
	local json rc
	json="$(mktemp -t gx-ios-devices)" || return 1
	if ! xcrun devicectl list devices --json-output "${json}" >/dev/null 2>&1; then
		rm -f "${json}"
		return 1
	fi
	# Single-quoted so bash leaves \t alone; every string below is double-quoted so the
	# quoting never collides with the shell's.
	python3 -c '
import json, sys

with open(sys.argv[1]) as fh:
    doc = json.load(fh)

for dev in doc.get("result", {}).get("devices", []) or []:
    hw = dev.get("hardwareProperties") or {}
    conn = dev.get("connectionProperties") or {}
    props = dev.get("deviceProperties") or {}
    # Tabs are the field separator and a device name is operator-supplied text, so scrub them.
    name = (props.get("name") or "").replace("\t", " ")
    print("\t".join([
        dev.get("identifier") or "",
        hw.get("udid") or "",
        hw.get("deviceType") or "",
        hw.get("platform") or "",
        conn.get("tunnelState") or "",
        name,
    ]))
' "${json}"
	rc=$?
	rm -f "${json}"
	return "${rc}"
}

# Pretty-print a newline-separated blob of TSV rows, one indented line each.
gx_ios_print_rows() {
	local id udid dtype platform tunnel name
	[[ -n "${1:-}" ]] || return 0
	while IFS=$'\t' read -r id udid dtype platform tunnel name; do
		[[ -n "${id}" ]] || continue
		printf '      %-24s %s  %s  (UDID %s)\n' "${name}" "${dtype}" "${id}" "${udid}"
	done <<< "$1"
}

# Resolve the install target. $1: a CoreDevice UUID, a hardware UDID, or "" to auto-select.
# Sets GX_DEVICE_UUID, GX_DEVICE_UDID, GX_DEVICE_TYPE and GX_DEVICE_NAME (cleared first).
# Returns 0 selected; 1 fatal (unreadable listing, or an explicit request that cannot be honoured); 2 no target chosen:
# a build-only caller may fall back to a generic destination, an installer must not.
gx_ios_select_device() {
	local want="${1:-}"
	local table matched id udid dtype platform tunnel name
	local pool="" ipads="" pool_n=0 ipads_n=0

	GX_DEVICE_UUID=""
	GX_DEVICE_UDID=""
	GX_DEVICE_TYPE=""
	GX_DEVICE_NAME=""

	# "generic": select nothing and touch no device - for packaging on a Mac that sees the iPad over the network while
	# another session holds the ipad lease (the signing profile must already list the device; installs refuse it).
	if [[ "${want}" == "generic" ]]; then
		echo "==> GX_IOS_DEVICE=generic: no device selected, none contacted." >&2
		return 2
	fi

	if ! table="$(gx_ios_device_table)"; then
		echo "ERROR: could not read the device list." >&2
		echo "  Tried: xcrun devicectl list devices --json-output <file>" >&2
		echo "  Needs Xcode's command line tools and python3." >&2
		return 1
	fi

	if [[ -n "${want}" ]]; then
		# Match on either identifier, case-insensitively: devicectl prints upper case, and an
		# operator pasting from notes may have either. Compared via tr rather than ${x,,},
		# which is bash 4 and this runs under /bin/bash 3.2 on macOS.
		local want_lc row_lc
		want_lc="$(printf '%s' "${want}" | tr '[:upper:]' '[:lower:]')"
		matched=""
		while IFS=$'\t' read -r id udid dtype platform tunnel name; do
			[[ -n "${id}" ]] || continue
			row_lc="$(printf '%s\t%s' "${id}" "${udid}" | tr '[:upper:]' '[:lower:]')"
			case "${row_lc}" in
				"${want_lc}"$'\t'*|*$'\t'"${want_lc}")
					matched="${id}"$'\t'"${udid}"$'\t'"${dtype}"$'\t'"${platform}"$'\t'"${tunnel}"$'\t'"${name}"
					break ;;
			esac
		done <<< "${table}"

		if [[ -z "${matched}" ]]; then
			echo "ERROR: no paired device matches the requested target '${want}'." >&2
			echo "    Paired devices right now:" >&2
			gx_ios_print_rows "${table}" >&2
			echo "    Give either identifier: the CoreDevice UUID (8-4-4-4-12) or the hardware" >&2
			echo "    UDID (8-16). Both are listed above." >&2
			return 1
		fi

		IFS=$'\t' read -r id udid dtype platform tunnel name <<< "${matched}"
		if [[ "${platform}" != "iOS" ]]; then
			echo "ERROR: requested device '${name}' runs ${platform:-an unknown platform} (${dtype})." >&2
			echo "    This app is iOS-only; it cannot be installed there." >&2
			return 1
		fi
		if [[ "${tunnel}" == "unavailable" ]]; then
			echo "ERROR: requested device '${name}' is paired but unavailable." >&2
			echo "    Connect it, unlock it, and make sure it trusts this computer." >&2
			return 1
		fi
		# Secondary guard only. The operator named this device, so being wrong about the model
		# family is their call to make - but it is worth saying out loud, because installing a
		# tablet build onto a phone is exactly the accident this file exists to prevent.
		if [[ "${dtype}" != "iPad" ]]; then
			echo "WARNING: requested device '${name}' has device type ${dtype}, not iPad." >&2
			echo "         Proceeding because it was named explicitly." >&2
		fi
		GX_DEVICE_UUID="${id}"
		GX_DEVICE_UDID="${udid}"
		GX_DEVICE_TYPE="${dtype}"
		GX_DEVICE_NAME="${name}"
		return 0
	fi

	# Auto-selection. The pool is what this app could conceivably install onto: an iOS device
	# whose tunnel is not reporting unavailable. Platform does that filtering, not model - a
	# paired Apple Watch is not a candidate under any naming scheme, and excluding it is
	# correctness, not the iPad guard.
	while IFS=$'\t' read -r id udid dtype platform tunnel name; do
		[[ -n "${id}" ]] || continue
		[[ "${platform}" == "iOS" ]] || continue
		[[ "${tunnel}" != "unavailable" ]] || continue
		local row="${id}"$'\t'"${udid}"$'\t'"${dtype}"$'\t'"${platform}"$'\t'"${tunnel}"$'\t'"${name}"
		pool="${pool}${row}"$'\n'
		pool_n=$((pool_n + 1))
		if [[ "${dtype}" == "iPad" ]]; then
			ipads="${ipads}${row}"$'\n'
			ipads_n=$((ipads_n + 1))
		fi
	done <<< "${table}"

	if [[ "${ipads_n}" -eq 1 ]]; then
		IFS=$'\t' read -r id udid dtype platform tunnel name <<< "${ipads}"
		GX_DEVICE_UUID="${id}"
		GX_DEVICE_UDID="${udid}"
		GX_DEVICE_TYPE="${dtype}"
		GX_DEVICE_NAME="${name}"
		if [[ "${pool_n}" -gt 1 ]]; then
			echo "==> Auto-selected the only paired iPad: ${name}" >&2
			echo "    ($((pool_n - 1)) other iOS device(s) paired and skipped. Set GX_IOS_DEVICE" >&2
			echo "     or pass --device to state the target instead of relying on this.)" >&2
		fi
		return 0
	fi

	if [[ "${pool_n}" -eq 0 ]]; then
		echo "==> No usable iOS device is paired with this Mac." >&2
		return 2
	fi

	echo "ERROR: refusing to guess the install target." >&2
	if [[ "${ipads_n}" -eq 0 ]]; then
		echo "    No paired iPad. These iOS devices are available, and none of them is one:" >&2
	else
		echo "    ${ipads_n} paired iPads are available and no target was stated:" >&2
	fi
	gx_ios_print_rows "${pool}" >&2
	echo "    State the target, by CoreDevice UUID or hardware UDID:" >&2
	echo "      scripts/build/ios/package-ios-zh.sh --device <UUID-or-UDID> [--dev] [--install]" >&2
	echo "    or set GX_IOS_DEVICE in scripts/test/local.env, which is untracked - see" >&2
	echo "    scripts/test/local.env.example. Env vars override the file; --device overrides both." >&2
	return 2
}
