#!/usr/bin/env python3
"""Compare native renderer experiments on an exactly verified local capture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


DEVICE_LOADER_MARKERS = ("DEVICE", "ICD", "LOADER", "LAYER", "DRIVER", "PHYSICAL",
                         "ADAPTER", "GPU_UUID", "GPU_PCI")
EXPLICIT_DEVICE_LOADER_KEYS = {
    "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_LAYER_PATH", "VK_INSTANCE_LAYERS",
    "DRI_PRIME", "__NV_PRIME_RENDER_OFFLOAD", "__GLX_VENDOR_LIBRARY_NAME",
}


def is_device_loader_selection(key):
    upper = key.upper()
    if key in EXPLICIT_DEVICE_LOADER_KEYS or upper.startswith("VK_LOADER_"):
        return True
    if upper.startswith(("XN_", "FORZA4_NATIVE_", "MCLA_NATIVE_")):
        return any(marker in upper for marker in DEVICE_LOADER_MARKERS)
    return False


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--variants", nargs="+", choices=("baseline", "state", "index", "both", "constants", "textures", "fallback", "float", "gpu", "multi", "all",
                                                            "fused", "prepared", "readonly", "frontendcombined",
                                                            "resident", "preparedstate", "residentstate", "dirtydraw", "dirtymulti"),
                        default=["baseline", "state", "index", "both"],
                        help="replay configurations; frontend and resident variants build on the established float/cache configuration")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100 or not 1 <= args.repeat <= 600:
        parser.error("rounds must be 1..100 and repeat must be 1..600")
    args.runner, args.trace, args.output = (p.resolve() for p in (args.runner, args.trace, args.output))
    args.output.mkdir(parents=True, exist_ok=False)
    # state, index reuse, constant reuse, textures, GPU ABI, multi-draw, cached fallback, float reuse
    configurations = {"baseline": (0, 0, 0, 0, 0, 0, 0, 0), "state": (1, 0, 0, 0, 0, 0, 0, 0),
                      "index": (0, 1, 0, 0, 0, 0, 0, 0), "both": (1, 1, 0, 0, 0, 0, 0, 0),
                      "constants": (0, 0, 1, 0, 0, 0, 0, 0), "textures": (1, 0, 0, 1, 0, 0, 0, 0),
                      "fallback": (1, 0, 0, 1, 0, 0, 1, 0), "float": (1, 0, 0, 1, 0, 0, 0, 1),
                      "gpu": (1, 0, 0, 1, 1, 0, 0, 1), "multi": (1, 0, 0, 1, 1, 1, 0, 1),
                      "all": (1, 1, 1, 0, 0, 0, 0, 0)}
    feature_flags = {key: {} for key in configurations}
    feature_flags.update({
        "fused": {"XN_NATIVE_GFX_BATCH": "1", "XN_CP_DRAW_STATE_BATCH": "1"},
        "prepared": {"XN_VK_PREPARED_INDEX_CACHE": "1"},
        "readonly": {"XN_VK_READONLY_BARRIER_ELISION": "1"},
        "frontendcombined": {"XN_NATIVE_GFX_BATCH": "1", "XN_CP_DRAW_STATE_BATCH": "1",
                             "XN_VK_PREPARED_INDEX_CACHE": "1", "XN_VK_READONLY_BARRIER_ELISION": "1"},
        "dirtydraw": {"XN_VK_DIRTY_DRAW_PREPARATION": "1"},
        "dirtymulti": {"XN_VK_DIRTY_DRAW_PREPARATION": "1", "XN_VK_GPU_CONSTANTS": "1", "XN_VK_MULTI_DRAW": "1"},
        "resident": {"XN_VK_RESIDENT_INDEX_CACHE": "1"},
        "preparedstate": {"XN_VK_PREPARED_DRAW_STATE": "1"},
        "residentstate": {"XN_VK_RESIDENT_INDEX_CACHE": "1", "XN_VK_PREPARED_DRAW_STATE": "1"},
    })
    for variant in ("fused", "prepared", "readonly", "frontendcombined", "resident", "preparedstate", "residentstate", "dirtydraw", "dirtymulti"):
        # Compare new front-end flags on top of the established float/cache
        # configuration, with no unrelated state changes from legacy variants.
        configurations[variant] = configurations["float"]
    provenance = dict(runner=str(args.runner), runner_sha256=digest(args.runner),
                      trace=str(args.trace), trace_sha256=digest(args.trace),
                      rounds=args.rounds, repeats_per_round=args.repeat,
                      configurations={key: configurations[key] for key in args.variants},
                      feature_flags={key: feature_flags[key] for key in args.variants},
                      preserved_device_loader_env={key: value for key, value in os.environ.items()
                                                   if is_device_loader_selection(key)},
                      timing_scope="prepared_target_events_including_memory_patches")
    (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    base_env = {key: value for key, value in os.environ.items()
                if not key.startswith(("XN_", "FORZA4_NATIVE_", "MCLA_NATIVE_"))}
    base_env.update({key: value for key, value in os.environ.items()
                     if is_device_loader_selection(key)})
    rows = {key: [] for key in args.variants}
    for round_index in range(args.rounds):
        order = args.variants if round_index % 2 == 0 else list(reversed(args.variants))
        for variant in order:
            state, index, constants, textures, gpu, multi, cached_fallback, floats = configurations[variant]
            env = dict(base_env, XN_VK_DRAW_STATE_CACHE=str(state), XN_VK_INDEX_REUSE=str(index),
                       XN_VK_CONSTANT_REUSE=str(constants), XN_VK_TEXTURE_DIRTY_CACHE=str(textures),
                       XN_VK_GPU_CONSTANTS=str(gpu), XN_VK_MULTI_DRAW=str(multi),
                       XN_VK_CACHED_UPLOAD_FALLBACK=str(cached_fallback), XN_VK_FLOAT_CONSTANT_REUSE=str(floats),
                       XN_VK_DIRTY_DRAW_PREPARATION="0")
            env.update(feature_flags[variant])
            stem = args.output / f"round-{round_index + 1}-{variant}"
            command = [str(args.runner), str(args.trace), "--repeat", str(args.repeat),
                       "--cache", str(args.output / "pipeline-cache"),
                       "--output", str(args.output / "mismatches")]
            with stem.with_suffix(".jsonl").open("w") as out, stem.with_suffix(".log").open("w") as log:
                result = subprocess.run(command, env=env, stdout=out, stderr=log)
            if result.returncode:
                raise RuntimeError(f"{variant} failed verification; timings rejected: {stem}")
            samples = [json.loads(line) for line in stem.with_suffix(".jsonl").read_text().splitlines()]
            if len(samples) != args.repeat or any(not row.get("exact_pixels") for row in samples):
                raise RuntimeError(f"{variant} has missing or unverified samples")
            for row in samples:
                row["round"] = round_index + 1
                row["steady_state"] = not row["pipeline_compiles"] and not row["shader_translations"]
            rows[variant].extend(samples)
            print(json.dumps(dict(round=round_index + 1, variant=variant,
                                  exact_pixels=True, samples=len(samples))), flush=True)
    metrics = ("replay_frame_cpu_ms", "replay_frame_wall_ms", "draws", "submissions", "cpu_fence_wait_ms",
               "gpu_completed_ms_delta", "draw_state_commands_saved", "index_cache_hits", "index_cache_misses",
               "fused_draw_callbacks",
               "converted_bytes_avoided", "index_ring_bytes_avoided", "constant_uploads_reused", "constant_upload_bytes",
               "texture_dirty_hits", "texture_dirty_misses", "gpu_constant_draws",
               "multi_draw_calls", "multi_draw_draws", "multi_draw_calls_saved",
               "constant_memory_flags", "index_memory_flags", "draw_record_memory_flags",
               "float_constant_uploads_reused", "float_constant_bytes_avoided",
               "resident_index_hits", "resident_index_misses", "resident_index_upload_bytes",
               "resident_index_bytes_reused", "resident_index_fallbacks",
               "preparation_shader_hits", "preparation_viewport_hits", "preparation_system_hits", "preparation_pipeline_hits", "dirty_constant_uploads_reused", "float_constant_packs_avoided",
               "prepared_shader_lookups_avoided", "prepared_pipeline_lookups_avoided")
    summary = {}
    for variant, samples in rows.items():
        stable = [row for row in samples if row["steady_state"]]
        if not stable:
            raise RuntimeError(f"{variant} has no samples without pipeline/shader compilation")
        fields = {}
        for metric in metrics:
            # Older replay binaries omit metrics added by later renderer builds.
            values = [row.get(metric, 0) for row in stable]
            median = statistics.median(values)
            fields[metric] = dict(median=median, minimum=min(values), maximum=max(values),
                                  mad=statistics.median(abs(value - median) for value in values))
        summary[variant] = dict(verified_samples=len(samples), steady_samples=len(stable), metrics=fields)
    output = dict(provenance=provenance, results=summary,
                  gpu_timing_scope="lagged_completed_batches; not exact target GPU duration")
    (args.output / "samples.json").write_text(json.dumps(rows, indent=2) + "\n")
    (args.output / "summary.json").write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
