#!/usr/bin/env python3
"""Read FMOD FSB4 sound banks: list streams, and extract them as WAV.

Every 360 title built on FMOD keeps its audio in FSB4 banks - Hydro Thunder in
Audio/Xbox360/*.fsb, Shift 2 in Audio/Music/*.fsb alongside .fev event files -
and the streams inside are almost always XMA2. Reading them on the host is the
first thing a native audio path needs, and the only way to get ground truth to
compare a title's own mixer output against.

  tools/fsb_tool.py list   <bank.fsb>
  tools/fsb_tool.py find   <bank.fsb> <substring>
  tools/fsb_tool.py extract <bank.fsb> <index|name> <out.wav>

Decoding uses vgmstream-cli (--vgmstream, or VGMSTREAM_CLI, or on PATH); the
header parsing here is ours, so listing and searching work without it.
"""
import argparse, os, shutil, struct, subprocess, sys

# The mode word, as FMOD writes it. Only the bits that change how a stream is
# read are named; the rest are playback hints.
FSOUND_LOOP_NORMAL = 0x00000002
FSOUND_8BITS       = 0x00000008
FSOUND_16BITS      = 0x00000010
FSOUND_MONO        = 0x00000020
FSOUND_STEREO      = 0x00000040
FSOUND_SIGNED      = 0x00000100
FSOUND_XMA         = 0x01000000  # confirmed against both titles' banks


class Stream:
    __slots__ = ("index", "name", "samples", "size", "loop_start", "loop_end",
                 "mode", "freq", "channels", "offset")

    @property
    def codec(self):
        if self.mode & FSOUND_XMA:
            return "XMA2"
        if self.mode & FSOUND_16BITS:
            return "PCM16"
        if self.mode & FSOUND_8BITS:
            return "PCM8"
        return f"mode {self.mode:#010x}"

    @property
    def seconds(self):
        return self.samples / self.freq if self.freq else 0.0


def read_bank(path):
    """Parse an FSB4 header and its sample table. Raises on anything else."""
    with open(path, "rb") as f:
        head = f.read(0x30)
        if len(head) < 0x30 or head[:4] != b"FSB4":
            raise ValueError(f"{path}: not an FSB4 bank")
        count, shdr_size, data_size, version, flags = struct.unpack_from("<IIIII", head, 4)
        table = f.read(shdr_size)

    streams, off, data_off = [], 0, 0x30 + shdr_size
    for i in range(count):
        if off + 64 > len(table):
            break
        size, = struct.unpack_from("<H", table, off)
        if size < 64 or off + size > len(table):
            break
        s = Stream()
        s.index = i + 1  # vgmstream numbers streams from 1
        s.name = table[off + 2:off + 32].split(b"\0")[0].decode("latin-1")
        (s.samples, s.size, s.loop_start, s.loop_end,
         s.mode, s.freq) = struct.unpack_from("<IIIIII", table, off + 32)
        s.channels, = struct.unpack_from("<H", table, off + 62)
        s.offset = data_off
        data_off += s.size
        streams.append(s)
        off += size
    return streams, {"count": count, "data_size": data_size, "version": version, "flags": flags}


def find_vgmstream(explicit):
    for candidate in (explicit, os.environ.get("VGMSTREAM_CLI"),
                      shutil.which("vgmstream-cli"),
                      "/home/jon/XboxRecompv2/RetroRecomp/tools/vgmstream-cli"):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=("list", "find", "extract"))
    ap.add_argument("bank")
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--vgmstream", default=None)
    args = ap.parse_args()

    streams, info = read_bank(args.bank)
    if args.action == "list":
        print(f"{os.path.basename(args.bank)}: {info['count']} streams, "
              f"{info['data_size'] / 1e6:.1f} MB of data")
        print(f"{'idx':>5} {'name':38} {'rate':>6} {'ch':>3} {'codec':>6} {'length':>8}  loop")
        for s in streams:
            loop = "yes" if s.mode & FSOUND_LOOP_NORMAL else "-"
            print(f"{s.index:5} {s.name[:38]:38} {s.freq:6} {s.channels:3} "
                  f"{s.codec:>6} {s.seconds:7.2f}s  {loop}")
        return 0

    if args.action == "find":
        if not args.rest:
            ap.error("find needs a substring")
        needle = args.rest[0].lower()
        hits = [s for s in streams if needle in s.name.lower()]
        for s in hits:
            print(f"{s.index:5} {s.name:38} {s.freq:6} Hz {s.channels}ch {s.codec} {s.seconds:.2f}s")
        if not hits:
            print(f"no stream matching '{args.rest[0]}'", file=sys.stderr)
            return 1
        return 0

    if len(args.rest) != 2:
        ap.error("extract needs <index|name> <out.wav>")
    wanted, out = args.rest
    if wanted.isdigit():
        match = next((s for s in streams if s.index == int(wanted)), None)
    else:
        needle = wanted.lower()
        match = next((s for s in streams if needle in s.name.lower()), None)
    if not match:
        print(f"no stream '{wanted}' in {args.bank}", file=sys.stderr)
        return 1

    cli = find_vgmstream(args.vgmstream)
    if not cli:
        print("vgmstream-cli not found; pass --vgmstream or set VGMSTREAM_CLI", file=sys.stderr)
        return 2
    subprocess.run([cli, "-s", str(match.index), "-o", out, args.bank], check=True,
                   stdout=subprocess.DEVNULL)
    print(f"{match.name}  {match.freq} Hz {match.channels}ch {match.codec} "
          f"{match.seconds:.2f}s -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
