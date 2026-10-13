"""Create deterministic gzip data for embedding the standalone control page."""
import gzip
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
output.parent.mkdir(parents=True, exist_ok=True)
data = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
if not output.exists() or output.read_bytes() != data:
    output.write_bytes(data)
