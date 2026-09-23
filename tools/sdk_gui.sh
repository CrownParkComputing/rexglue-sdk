#!/usr/bin/env bash
# The SDK dashboard: every port, how far each one is, and the buttons that move
# them along - including handing a job to the agent.
#
#   tools/sdk_gui.sh
#
# The point of this existing is that converting a title should not be someone
# typing commands from memory. A first boot is quick and the GUI can drive it;
# what needs a person (or the agent) is a FAILURE, and the specific judgement
# of where a title can go native. Both of those are buttons here, and the
# "Ask the agent" one writes a job file rather than pretending it can summon
# anybody: the queue is where the next session looks.
set -uo pipefail

SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PORTS="${REXPORTS_DIR:-/home/jon/recomp-ports}"
QUEUE="$SDK/agent-queue"
mkdir -p "$QUEUE"

command -v zenity >/dev/null 2>&1 || { echo "zenity is required" >&2; exit 2; }
[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || { echo "no display" >&2; exit 2; }

REPORT="$(mktemp "${TMPDIR:-/tmp}/rexglue-sdk.XXXXXX")"
trap 'rm -f "$REPORT"' EXIT

port_dirs() { find "$PORTS" -maxdepth 1 -name '*-recomp' -type d | sort; }

build_table() {
  {
    echo "ReXGlue SDK - conversion estate"
    echo "==============================="
    echo
    printf "  %-20s %-4s %-7s %-7s %-7s %-8s %-6s\n" \
           "port" "game" "codegen" "build" "verify" "traced" "native"
    printf "  %-20s %-4s %-7s %-7s %-7s %-8s %-6s\n" \
           "--------------------" "----" "-------" "-------" "-------" "--------" "------"
    local n=0 built=0 verified=0 traced=0
    while read -r d; do
      [ -n "$d" ] || continue
      line="$(python3 "$SDK/tools/pipeline.py" "$d" fast 2>/dev/null)"
      [ -n "$line" ] || continue
      IFS='|' read -r slug game gen bin peak tr nat <<<"$line"
      n=$((n+1))
      [ "$bin" != "-" ] && built=$((built+1))
      # A verify only counts when the run left the menus: a menu issues tens of
      # draws, a level hundreds. This is the check that would have caught a port
      # being called finished off its intro screen.
      local vshow="$peak"
      if [ "$peak" != "-" ]; then
        if [ "${peak%.*}" -ge 150 ] 2>/dev/null; then vshow="$peak ok"; verified=$((verified+1));
        else vshow="$peak LOW"; fi
      fi
      [ "$tr" != "-" ] && traced=$((traced+1))
      printf "  %-20s %-4s %-7s %-7s %-7s %-8s %-6s\n" \
             "$slug" "$game" "$gen" "$bin" "$vshow" "$tr" "$nat"
    done < <(port_dirs)
    echo
    echo "  $n ports   $built built   $verified verified into gameplay   $traced traced"
    echo
    echo "  game    = the disc/XBLA files are present (not that they verify)"
    echo "  verify  = peak draws/frame in the last driven run; LOW means it"
    echo "            probably never left the menus, so it is NOT verified"
    echo "  traced  = kernel imports the title ACTUALLY calls (REX_TRACE_IMPORTS)."
    echo "            Without this the native % is measured against the XEX table,"
    echo "            which overstates the work - Sega Rally imports 347, calls 123."
    echo "  native  = of those called imports, how many the port defines itself."
    echo "            This is the goal and it is the long tail, not a tick-box."
    echo
    if [ -n "$(ls -A "$QUEUE" 2>/dev/null)" ]; then
      echo "  jobs waiting for the agent:"
      for f in "$QUEUE"/*.job; do [ -f "$f" ] && echo "    - $(basename "$f")"; done
    fi
  } > "$REPORT"
}

pick_port() {
  local rows=() d line slug
  while read -r d; do
    [ -n "$d" ] || continue
    line="$(python3 "$SDK/tools/pipeline.py" "$d" fast 2>/dev/null)"
    IFS='|' read -r slug _ _ bin peak _ nat <<<"$line"
    rows+=("$slug" "$(basename "$d")" "${bin:--}" "${nat:--}")
  done < <(port_dirs)
  zenity --list --title="Pick a port" --width=560 --height=520 \
    --column="port" --column="folder" --column="build" --column="native" \
    "${rows[@]}" 2>/dev/null
}

run_stage() {  # dir, stage
  local d="$1" st="$2" log="$1/out/pipeline.log"
  mkdir -p "$(dirname "$log")"
  ( if [ "$st" = "all" ] || [ "$st" = "next" ]; then python3 "$SDK/tools/pipeline.py" "$d" "$st"; else python3 "$SDK/tools/pipeline.py" "$d" run "$st"; fi >"$log" 2>&1; echo $? >"$log.rc" ) &
  local pid=$!
  zenity --progress --pulsate --auto-close --no-cancel --width=480 \
    --title="$(basename "$d")" \
    --text="Running: $st\n\nBuilds and verification runs take minutes.\nThe window will close when it finishes." 2>/dev/null &
  local bar=$!
  wait $pid; kill $bar 2>/dev/null
  if [ "$(cat "$log.rc" 2>/dev/null || echo 1)" = "0" ]; then
    zenity --info --no-wrap --title="$(basename "$d")" --text="$(tail -3 "$log")" 2>/dev/null
  else
    if zenity --question --no-wrap --title="$(basename "$d")" \
         --text="$(tail -4 "$log")\n\nLog: $log\n\nHand this failure to the agent?" 2>/dev/null; then
      queue_job "$(basename "$d")" "failure in stage '$st'" \
        "$(tail -40 "$log")" "$log"
    fi
  fi
}

queue_job() {  # port, title, body, logpath
  local f="$QUEUE/$(date +%Y%m%d-%H%M%S)-${1}.job"
  {
    echo "port:    $1"
    echo "job:     $2"
    echo "raised:  $(date -Is)"
    [ -n "${4:-}" ] && echo "log:     $4"
    echo
    echo "--- detail ---"
    echo "$3"
  } > "$f"
  zenity --info --no-wrap --title="Queued" \
    --text="Job written for the agent:\n\n$f\n\nIt will pick this up next session." 2>/dev/null
}

while :; do
  build_table
  OUT="$(zenity --text-info --title="ReXGlue SDK" --filename="$REPORT" \
        --width=900 --height=680 --font="monospace 10" \
        --ok-label="Port actions" --cancel-label="Close" \
        --extra-button="Native targets" \
        --extra-button="Ask the agent" \
        --extra-button="Refresh" 2>/dev/null)"
  RC=$?
  case "$OUT" in
    *"Refresh"*) continue ;;
    *"Native targets"*)
      T="$(mktemp)"; python3 "$SDK/tools/native_targets.py" >"$T" 2>&1
      G="$(zenity --text-info --title="Native targets" --filename="$T" \
            --width=900 --height=680 --font="monospace 10" \
            --ok-label="Close" --extra-button="Send a group to the agent" 2>/dev/null)"
      if [ "$G" = "Send a group to the agent" ]; then
        GRP="$(zenity --list --title="Which group?" --width=420 --height=380 \
              --column="group" "file I/O" "threads / TLS" "sync / events" "memory" \
              "string / rtl" "locale / system" "input" "profile / saves" "audio" 2>/dev/null)"
        if [ -n "$GRP" ]; then
          D="$(python3 "$SDK/tools/native_targets.py" --group "$GRP" 2>&1)"
          queue_job "estate" "go native on: $GRP" "$D" ""
        fi
      fi
      rm -f "$T"; continue ;;
    *"Ask the agent"*)
      MSG="$(zenity --entry --width=560 --title="Ask the agent" \
             --text="What should the agent do?" 2>/dev/null)"
      [ -n "$MSG" ] && queue_job "estate" "$MSG" "(raised from the SDK dashboard)" ""
      continue ;;
  esac
  [ "$RC" -eq 0 ] || exit 0

  D="$(pick_port)"; [ -n "$D" ] || continue
  DIR="$PORTS/$D-recomp"; [ -d "$DIR" ] || DIR="$PORTS/$D"
  while :; do
    S="$(python3 "$SDK/tools/pipeline.py" "$DIR" status 2>/dev/null)"
    A="$(zenity --list --title="$D" --width=640 --height=520 \
        --text="$S" --column="action" \
        "Run ALL remaining stages" "Run next stage" "Verify (drive into gameplay)" "Trace imports" \
        "Measure native" "Import game files" "Play" "Open port GUI" "Back" 2>/dev/null)"
    case "$A" in
      "Run ALL remaining stages") run_stage "$DIR" all ;;
      "Run next stage")  run_stage "$DIR" next ;;
      "Verify"*)         run_stage "$DIR" verify ;;
      "Trace imports")   run_stage "$DIR" trace ;;
      "Measure native")  run_stage "$DIR" native ;;
      "Import game files") "$DIR/tools/import_content.sh" || true ;;
      "Play")            ( cd "$DIR" && ./run.sh & ) ; break ;;
      "Open port GUI")   ( "$DIR/tools/port_gui.sh" & ) ; break ;;
      *) break ;;
    esac
  done
done
