"""Deterministically compress the embedded portal for browser delivery."""
import gzip
from pathlib import Path
import sys
source,output=map(Path,sys.argv[1:])
encoded=gzip.compress(source.read_bytes(),compresslevel=9,mtime=0)
assert gzip.decompress(encoded)==source.read_bytes()
output.write_bytes(encoded)
