#!/usr/bin/env bash
# reach_from_perf.sh - CPU-sample weights for guest functions in the reach CSV format.
#
#   reach_from_perf.sh record [-o perf.data] [-F HZ] [--no-callgraph] -- <cmd...>
#   reach_from_perf.sh convert <perf.data|perf-script.txt> [-o reach_perf.csv] [--inclusive]
#   reach_from_perf.sh run    [-o reach_perf.csv] -- <cmd...>      # record + convert
#
# `record` wraps `perf record -g -- <cmd>` (frame pointers; rexglue builds keep
# them in generated code). `convert` runs `perf script` and folds every sample
# whose symbol is `sub_XXXXXXXX` or `__imp__sub_XXXXXXXX` into
#
#   addr,count
#   823DDA50,4812
#
# which is byte-compatible with the REX_TRACE_REACH CSV (docs/native/FORMATS.md),
# so native_report.py / native_check.py read either. The difference is the
# meaning of `count`: here it is CPU samples (self cost by default; with
# --inclusive every distinct guest frame in the call chain is credited once
# per sample, i.e. inclusive cost), not calls. Pass it to native_report.py as
# --perf and to native_check.py as --reach (any sample in a range proves the
# range was reached).
#
# Symbols other than sub_X (runtime, kernel HLE, libc) are counted in the
# `# other_samples=` trailer comment so the guest share of CPU can be reported.
# Environment: PERF (default `perf`).
set -euo pipefail

PERF="${PERF:-perf}"

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

die() { echo "reach_from_perf: $*" >&2; exit 2; }

cmd_record() {
    local out="perf.data" freq="" cg="-g"
    while [ $# -gt 0 ]; do
        case "$1" in
            -o) out="$2"; shift 2 ;;
            -F) freq="-F $2"; shift 2 ;;
            --no-callgraph) cg=""; shift ;;
            --) shift; break ;;
            -h|--help) usage ;;
            *) die "record: unknown option $1" ;;
        esac
    done
    [ $# -gt 0 ] || die "record: no command after --"
    # shellcheck disable=SC2086
    "$PERF" record $cg $freq -o "$out" -- "$@" || true   # a killed game still leaves usable data
    echo "reach_from_perf: wrote $out" >&2
}

# perf script output (with -g):
#   comm  pid  time:  period  event:
#   \t            addr sym+off (dso)
#   \t            addr sym+off (dso)
#   <blank line>
# without -g: one line per sample "comm pid time: period event: addr sym+off (dso)".
# We only look for the sub_XXXXXXXX token anywhere on a frame line.
convert_stream() {
    local inclusive="$1"
    awk -v inclusive="$inclusive" '
        function flush() {
            if (nframes == 0) return
            if (inclusive) {
                if (nseen == 0) other++
                for (a in seen) count[a]++
            } else if (first != "") {
                count[first]++
            } else {
                other++
            }
            nframes = 0; first = ""; nseen = 0; delete seen
        }
        /^[^ \t]/ && !/^#/ {            # sample header (or a whole one-line sample)
            flush(); nframes = 0
            if (match($0, /(__imp__)?sub_[0-9A-Fa-f]{8}/)) {
                a = toupper(substr($0, RSTART + RLENGTH - 8, 8))
                count[a]++; samples++
            } else if ($0 ~ /\(/ && $0 ~ /:/ && $0 !~ /event:$/ && $0 !~ /:[ \t]*$/) {
                # one-line (no callchain) sample with a non-guest symbol
                other++; samples++
            } else {
                header = 1; samples++
            }
            next
        }
        /^[ \t]+[0-9a-fA-F]+[ \t]/ {    # a frame line
            nframes++
            if (match($0, /(__imp__)?sub_[0-9A-Fa-f]{8}/)) {
                a = toupper(substr($0, RSTART + RLENGTH - 8, 8))
                if (nframes == 1) first = a          # self cost: only the leaf frame counts
                if (!(a in seen)) nseen++
                seen[a] = 1
            }
            next
        }
        /^[ \t]*$/ { flush(); next }
        END {
            flush()
            print "addr,count"
            n = 0
            for (a in count) { n++; line[n] = sprintf("%s,%d", a, count[a]); key[n] = count[a] }
            # sort by count desc (simple insertion sort; n is small)
            for (i = 2; i <= n; i++) { v = line[i]; k = key[i]; j = i - 1
                while (j > 0 && key[j] < k) { line[j+1] = line[j]; key[j+1] = key[j]; j-- }
                line[j+1] = v; key[j+1] = k }
            for (i = 1; i <= n; i++) print line[i]
            printf "# samples=%d guest_rows=%d other_samples=%d mode=%s\n", samples, n, other, (inclusive ? "inclusive" : "self")
        }'
}

cmd_convert() {
    local out="reach_perf.csv" inclusive=0 input=""
    while [ $# -gt 0 ]; do
        case "$1" in
            -o) out="$2"; shift 2 ;;
            --inclusive) inclusive=1; shift ;;
            -h|--help) usage ;;
            -*) die "convert: unknown option $1" ;;
            *) input="$1"; shift ;;
        esac
    done
    [ -n "$input" ] && [ -f "$input" ] || die "convert: input file required"
    local tmp="$out.tmp"
    if head -c 8 "$input" | grep -q "PERFILE2"; then
        "$PERF" script -i "$input" -F comm,pid,time,period,event,ip,sym,dso 2>/dev/null | convert_stream "$inclusive" > "$tmp"
    else
        convert_stream "$inclusive" < "$input" > "$tmp"          # already-textual perf script output
    fi
    mv "$tmp" "$out"
    tail -n 1 "$out" | sed 's/^# /reach_from_perf: /' >&2
    echo "reach_from_perf: wrote $out" >&2
}

cmd_run() {
    local out="reach_perf.csv" data
    while [ $# -gt 0 ]; do
        case "$1" in
            -o) out="$2"; shift 2 ;;
            --) shift; break ;;
            *) die "run: unknown option $1" ;;
        esac
    done
    data="${out%.csv}.perf.data"
    cmd_record -o "$data" -- "$@"
    cmd_convert "$data" -o "$out"
}

[ $# -gt 0 ] || usage
sub="$1"; shift
case "$sub" in
    record)  cmd_record "$@" ;;
    convert) cmd_convert "$@" ;;
    run)     cmd_run "$@" ;;
    *) usage ;;
esac
