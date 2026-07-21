#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Generic, zero-dependency syslog and file-slicing verification helper library.

# Default APP to DYNDBG if not already set
APP="${APP:-DYNDBG}"
APP_LOWER=$(echo "$APP" | tr '[:upper:]' '[:lower:]')

# Global files for tracking seen, unregistered, and drifted hashes
SEEN_HASHES_FILE="/tmp/${APP_LOWER}_seen_hashes_$$"
UNREG_HASHES_FILE="/tmp/${APP_LOWER}_unreg_hashes_$$"
DRIFT_HASHES_FILE="/tmp/${APP_LOWER}_drift_hashes_$$"

# Global variables for tracking active function transitions and sequence resets
LAST_FT_FUNC=""
TEST_SEQ_CTR=0
ACTIVE_RESOLVED_LABEL=""

# Global variables for bookending state transitions and local stimulus tracking
IN_BOOKEND=0
LOCAL_DDCMDS=""

# Global variable to track the active dmesg block label
ACTIVE_LOG_LABEL=""

# Helper function to auto-resolve active FT_ test and sequence label
function rdi_resolve_label {
    local caller_fn=""
    # Traverse the call stack to find the active Feature Test function (FT_*)
    for fn in "${FUNCNAME[@]}"; do
        if [[ "$fn" == FT_* ]]; then
            caller_fn="$fn"
            break
        fi
    done

    # Fallback to the immediate caller if no FT_ is in the stack
    if [ -z "$caller_fn" ]; then
        caller_fn="${FUNCNAME[1]:-}"
    fi

    # Automatically reset sequence counter if the executing function has transitioned
    if [ -n "$caller_fn" ] && [ "$caller_fn" != "$LAST_FT_FUNC" ]; then
        TEST_SEQ_CTR=1
        LAST_FT_FUNC="$caller_fn"
    fi

    if [ -n "$caller_fn" ]; then
        ACTIVE_RESOLVED_LABEL="${caller_fn}.${TEST_SEQ_CTR}"
    else
        ACTIVE_RESOLVED_LABEL="${TEST_SEQ_CTR}"
    fi
}

function log_start {
    ((TEST_SEQ_CTR++))

    rdi_resolve_label
    ACTIVE_LOG_LABEL="$ACTIVE_RESOLVED_LABEL"

    IN_BOOKEND=1

    echo "${APP}_START_${ACTIVE_LOG_LABEL}_$$" > /dev/kmsg
}

function log_stop {
    # Ends the dmesg capture block and verifies the slice
    if [ -z "$ACTIVE_LOG_LABEL" ]; then
        echo "Error: log_stop called without a matching log_start!" >&2
        return 1
    fi

    echo "${APP}_END_${ACTIVE_LOG_LABEL}_$$" > /dev/kmsg

    # Verify the dmesg slice
    verify_dmesg_slice "$ACTIVE_LOG_LABEL"

    # Reset active state, bookend flag, and command tracker at teardown
    ACTIVE_LOG_LABEL=""
    IN_BOOKEND=0
    LOCAL_DDCMDS=""
}

function verify_fingerprint {
    # Verifies a calculated fingerprint against the GOLDEN_RECORDS database
    # $1 - unique test key (e.g. normal_513)
    # $2 - optional extra args (e.g. "dmesg:1" or "control:pattern")
    # $3 - the calculated fingerprint hash to verify
    # $4 - description of what was captured (e.g. "Dmesg Log" or "File Slice")
    # $5 - the raw captured text block (to display in case of mismatch)

    local label="$1"
    local extra_args="$2"
    local fingerprint="$3"
    local capture_desc="$4"
    local raw_capture="$5"

    # Require GOLDEN_RECORDS to be defined in the caller script
    if ! declare -f GOLDEN_RECORDS >/dev/null; then
        echo "Error: GOLDEN_RECORDS() is not defined in the caller script." >&2
        return 1
    fi

    # Resolve the expected hash specifically for this label
    local expected_hash_field
    expected_hash_field=$(GOLDEN_RECORDS | \
        grep -E "[[:space:]]${label}[[:space:]]" | head -n1 | awk '{print $2}')

    local matched=0
    local h
    local OLD_IFS="$IFS"
    IFS=","
    for h in $expected_hash_field; do
        if [ "$h" = "$fingerprint" ]; then
            matched=1
            break
        fi
    done
    IFS="$OLD_IFS"

    # Strictly verify that the computed fingerprint matches any
    # expected hash for this label
    if [ -n "$expected_hash_field" ] && [ $matched -eq 1 ]; then
        local short_hash="${fingerprint:0:12}"
        [ "$V" -ge 1 ] && echo -e "${GREEN}✔ Verified '${label}' " \
            "(${short_hash}) [via: '${LOCAL_DDCMDS}']${NC}"

        if [ "$V" -ge 2 ]; then
            echo -e "${CYAN}--- Captured Invariant ${capture_desc} Output ($label) ---"
	    printf "#K= %-32s %-24s\n" "${fingerprint}" "${label}"
            echo "$raw_capture"
            echo -e "-----------------------------------${NC}"
        fi
        echo "$fingerprint" >> "$SEEN_HASHES_FILE"

        # Telemetry
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint VERIFIED $extra_args [via:\"$LOCAL_DDCMDS\"]"
        fi
    else
        # Failure path: display mismatch and append to corrections
        local status_str="UNREGISTERED"
        local stimulus="${LOCAL_DDCMDS:-direct write to control}"
        if [ -n "$expected_hash_field" ]; then
            local short_expected="${expected_hash_field:0:12}"
            local short_got="${fingerprint:0:12}"
            echo -e "${RED}: DRIFT for '${label}'${NC}"
            echo -e "  Range:     ${extra_args}"
            echo -e "  Stimulus:  ${stimulus}"
            echo -e "  Expected:  '${short_expected}' (${expected_hash_field})"
            echo -e "  Got:       '${short_got}' (${fingerprint})${NC}"
            status_str="DRIFTED"
        else
            echo -e "${YELLOW}: NO RECORD for '${label}'${NC}"
            echo -e "  Range:     ${extra_args}"
            echo -e "  Stimulus:  ${stimulus}${NC}"
        fi

        # Telemetry
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint $status_str $extra_args " \
                "[via:\"$CUMULATIVE_DDCMDS\"]"
        fi

        echo -e "\nAdd or replace this line in GOLDEN_RECORDS():"
        printf "#K= %-32s %-24s %s\n" "${fingerprint}" "${label}" "${extra_args}"
        echo -e "\n--- Captured Invariant ${capture_desc} Output ---"
        if [ "$capture_desc" = "File Slice" ]; then
            echo "$raw_capture" | \
                sed -E "s/ =([_a-z]*[a-z][_a-z]*) / ${YELLOW}=\1${NC} /g"
        else
            echo "$raw_capture"
        fi
        echo -e "-----------------------------------${NC}"

        if [ "$status_str" = "DRIFTED" ]; then
            printf "#K= %-32s %-24s %s\n" \
                "${fingerprint}" "${label}" "${extra_args}" \
                >> "$DRIFT_HASHES_FILE"
        else
            printf "#K= %-32s %-24s %s\n" \
                "${fingerprint}" "${label}" "${extra_args}" \
                >> "$UNREG_HASHES_FILE"
        fi
    fi
}

function verify_dmesg_slice {
    # Slices dmesg, computes its hash, and verifies it against the database.
    # $1 - unique test key (e.g. normal_513)
    # $2 - optional start marker (defaults to ${APP}_START_${label})
    # $3 - optional end marker (defaults to ${APP}_END_${label})

    local label="$1"
    local app="${APP:-DYNDBG}"
    local start_marker="${2:-${app}_START_${label}_$$}"
    local end_marker="${3:-${app}_END_${label}_$$}"
    local extra_args="dmesg"

    # 1. Capture the log slice (exactly once!)
    local log_slice=$(dmesg | sed -n "/$start_marker/,/$end_marker/p" | \
	grep -E -v "$start_marker|$end_marker" | \
        sed -e 's/^\[[^]]*\] //' )

    # 2. Compute its fingerprint
    local fingerprint=$(echo "$log_slice" | tr -d '\r' | md5sum | cut -d' ' -f1)

    # 3. Verify
    verify_fingerprint "$label" "$extra_args" "$fingerprint" "Dmesg Log" "$log_slice"
}

function slice_by_grep {
    # Isolate lines matching a pattern from a file
    # $1 - pattern to grep (returns entire file if empty or "*")
    # $2 - file path (reads $CONTROL_FILE if not provided)
    local pattern="$1"
    local file_path="${2:-$CONTROL_FILE}"

    if [ -z "$pattern" ] || [ "$pattern" = "*" ]; then
        cat "$file_path"
    else
        grep "$pattern" "$file_path"
    fi
}

function verify_file_slice {
    # Captures a file slice by pattern, computes its hash,
    # and verifies it against the database.
    # $1 - pattern to slice
    # $2 - optional file path (defaults to $CONTROL_FILE)
    # $3 - optional extra args

    local pattern="$1"
    local file="${2:-$CONTROL_FILE}"
    local extra_args="$3"

    # Always auto-resolve label via call stack sequence resets!
    ((TEST_SEQ_CTR++))
    rdi_resolve_label
    local label="$ACTIVE_RESOLVED_LABEL"

    if [ -z "$extra_args" ]; then
        extra_args="$pattern"
        if [ "$pattern" != "*" ]; then
            extra_args="\"$pattern\""
        fi
    fi

    # 1. Capture the file slice (exactly once!)
    local slice=$(slice_by_grep "$pattern" "$file")

    # 2. Compute its fingerprint
    local fingerprint=$(echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1)

    # 3. Verify
    verify_fingerprint "$label" "$extra_args" "$fingerprint" "File Slice" "$slice"
}

# Global variables for bookending state transitions
BEFORE_CAPTURE_SLICE=""
BEFORE_CAPTURE_PATTERN=""
BEFORE_CAPTURE_FILE=""

function capture_before {
    # Captures and stores the 'before' state for a file slice transition
    # $1 - pattern to slice
    # $2 - optional file path (defaults to $CONTROL_FILE)

    BEFORE_CAPTURE_PATTERN="$1"
    BEFORE_CAPTURE_FILE="${2:-$CONTROL_FILE}"
    BEFORE_CAPTURE_SLICE=$(slice_by_grep "$BEFORE_CAPTURE_PATTERN" "$BEFORE_CAPTURE_FILE")

    IN_BOOKEND=1
}

function verify_after_change {
    # Verifies the transition between the stored 'before' state and the current state
    # $1 - optional unique test key (resolved via stack if empty)

    local label="$1"

    if [ -z "$label" ]; then
        ((TEST_SEQ_CTR++))
        rdi_resolve_label
        label="$ACTIVE_RESOLVED_LABEL"
    fi
    local extra_args="$BEFORE_CAPTURE_PATTERN"
    if [ "$BEFORE_CAPTURE_PATTERN" != "*" ]; then
        extra_args="\"$BEFORE_CAPTURE_PATTERN\""
    fi

    if [ -z "$BEFORE_CAPTURE_PATTERN" ]; then
        echo "Error: verify_after_change called without a matching capture_before!" >&2
        return 1
    fi

    # 1. Capture the 'after' state (exactly once!)
    local after_slice=$(slice_by_grep "$BEFORE_CAPTURE_PATTERN" "$BEFORE_CAPTURE_FILE")

    # 2. Generate the unified diff, stripped of volatile diff headers AND hunk line-numbers
    local transition_diff=$(diff -u <(echo "$BEFORE_CAPTURE_SLICE") <(echo "$after_slice") | \
        tail -n +3 | \
        sed -E 's/^@@ -[0-9]+.* \+[0-9]+.* @@/@@/g')

    # 3. Compute its fingerprint
    local fingerprint=$(echo "$transition_diff" | tr -d '\r' | md5sum | cut -d' ' -f1)

    # 4. Verify the diff as the captured text block
    verify_fingerprint "$label" "$extra_args" "$fingerprint" "File Change Diff" "$transition_diff"

    # Reset state, bookend flag, and command tracker at teardown
    BEFORE_CAPTURE_SLICE=""
    BEFORE_CAPTURE_PATTERN=""
    BEFORE_CAPTURE_FILE=""
    IN_BOOKEND=0
    LOCAL_DDCMDS=""
}

function audit_golden_records {
    local seen_file="$SEEN_HASHES_FILE"

    if [ ! -f "$seen_file" ]; then
        return
    fi

    # Require GOLDEN_RECORDS to be defined in the caller script
    if ! declare -f GOLDEN_RECORDS >/dev/null; then
        return
    fi

    [ "${V:-0}" -ge 1 ] && echo -e "${YELLOW}# --- GOLDEN_RECORDS Audit ---${NC}"
    local stale_found=0
    local total_records=$(GOLDEN_RECORDS | grep -c "^#K=")

    # Read each active record line from GOLDEN_RECORDS
    while read -r line; do
        # Extract the hash/hashes (second word) from the #K= line
        local hash_field=$(echo "$line" | awk '{print $2}')

        # Check if at least one of the comma-separated hashes was seen
        local hash_seen=0
        local h
        local OLD_IFS="$IFS"
        IFS=","
        for h in $hash_field; do
            if grep -q "$h" "$seen_file" 2>/dev/null; then
                hash_seen=1
                break
            fi
        done
        IFS="$OLD_IFS"

        # Check if this hash field was seen during the run
        if [ $hash_seen -eq 0 ]; then
            if [ $stale_found -eq 0 ]; then
                # On first failure, print header if not already printed
                [ "${V:-0}" -eq 0 ] && \
                    echo -e "${YELLOW}# --- GOLDEN_RECORDS Audit ---${NC}"
                echo -e "${YELLOW}# The following GOLDEN_RECORDS entries " \
                    "were never hit and may be stale:${NC}"
                stale_found=1
            fi
            echo -e "${YELLOW}#K_STALE= $line${NC}"
        fi
    done < <(GOLDEN_RECORDS | grep "^#K=" | grep -v "<md5_hash>")

    if [ $stale_found -eq 0 ] && [ "${V:-0}" -ge 1 ]; then
        echo -e "${GREEN}# All $total_records GOLDEN_RECORDS entries " \
            "were successfully hit!${NC}"
    fi

    # Detect duplicate labels in the database
    local dupes=$(GOLDEN_RECORDS | grep "^#K=" | awk '{print $3}' | sort | uniq -d)
    if [ -n "$dupes" ]; then
        echo -e "\n${RED}# WARNING: Duplicate labels detected in GOLDEN_RECORDS():${NC}"
        echo "$dupes" | sed 's/^/#   /'
    fi

    # Clean up
    rm -f "$seen_file"
}
