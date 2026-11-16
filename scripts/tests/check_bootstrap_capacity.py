#!/usr/bin/env python3
"""Check native bootstrap table allocation against the marble 8 GiB handoff."""
import argparse
import json
from pathlib import Path
import re
import struct

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,required=True)
parser.add_argument('--bootargs',type=Path,required=True)
parser.add_argument('--tables',type=Path,required=True)
a=parser.parse_args()
text=(a.source/'osfmk/arm/pmap/pmap.h').read_text()
arm4k=text.split('#elif !defined(APPLE_ARM64_ARCH_FAMILY) && !__ARM_16K_PG__',1)[1].split('#else',1)[0]
capacity=int(re.search(r'BOOTSTRAP_TABLE_SIZE \(ARM_PGBYTES \* (\d+)\)',arm4k)[1])
args=a.bootargs.read_bytes();virtual,physical,window=struct.unpack_from('<3Q',args,8)
# start.s adjusts the incoming bases to the member Mach-O header before mapping.
# Member-header offsets stay inside the first 1 GiB region of each map here.
region=1<<30
physical_pages=((physical%region)+window+region-1)//region
virtual_pages=((virtual%region)+window+region-1)//region
required=2+physical_pages+virtual_pages
assert required==20 and capacity>=required,(required,capacity)
source=(a.source/'osfmk/arm64/start.s').read_text()
failure=source.split('Lbootstrap_tables_exhausted:',1)[1].split('Lbootstrap_tables_ready:',1)[0]
assert 'br x16' in failure and '[x20, #0x808]' in failure and 'smc #0' in failure
assert 'b Lbootstrap_tables_exhausted' not in failure
old=a.tables.read_bytes()
root_counts=[sum((value&3)==3 for value in struct.unpack_from('<512Q',old,p*4096)) for p in (0,1)]
assert root_counts==[9,5],root_counts
print(json.dumps({'physical_tables':physical_pages,'virtual_tables':virtual_pages,'required_pages':required,
                  'capacity_pages':capacity,'observed_old_roots':root_counts,
                  'failure_path':'firmware return with PSCI fallback'},indent=2))
