# Write-support validation: LINE, LWPOLYLINE, CIRCLE, ARC, TEXT, LAYER, INSERT

**Question:** does libdxfrw actually support *creating* these entities, for
both DXF and DWG, or only reading them?

**Answer: yes, for both.** Verified empirically, not just by reading the
source — `write_test.cpp` builds each entity in memory and writes real
`.dxf` and `.dwg` files; `read_test.cpp` reads them back with a fresh
`DRW_Interface` implementation and prints every field. All seven came back
exactly as written, in both formats:

```
LAYER: name='MYLAYER' color=3
BLOCK: name='MYBLOCK' base=(0.000,0.000)
LINE: (0.000,0.000) -> (100.000,50.000) layer='MYLAYER'
CIRCLE: center=(50.000,50.000) r=20.000 layer='MYLAYER'
ARC: center=(120.000,20.000) r=15.000 start=0.000 end=3.142 layer='MYLAYER'
LWPOLYLINE: 4 verts closed=1 layer='MYLAYER' [(0.000,80.000) (30.000,80.000) (30.000,110.000) (0.000,110.000)]
TEXT: 'HELLO' at=(150.000,0.000) height=5.000 layer='MYLAYER'
INSERT: block='MYBLOCK' at=(200.000,200.000) layer='MYLAYER'
```

`file roundtrip_out.dwg` reports `DWG AutoDesk AutoCAD 2000` — a genuine
binary DWG with correct magic bytes (`AC1015`), not a mislabeled DXF.

## Things that will bite you if you write your own export code

1. **`DRW::AC1021` fails for DWG output with `BAD_VERSION`.** The DWG
   writer is gated to a specific fixed set of target versions —
   `AC1015`/`AC1018`/`AC1024`/`AC1027`/`AC1032` — not the full DXF version
   range. Per this repo's own `DWG_DXF_WRITER_SUPPORT_STATUS.md`, **AC1015
   is currently the only one described as fully validated/"hard-gated"**;
   the others exist but are still being promoted through validation.
   DXF output doesn't have this restriction. **Use `DRW::AC1015` for DWG
   writes** until upstream promotes a later version.
2. **`dxfRW::writeBlock()` will segfault if you call it before
   `writeBlockRecord()`.** It does an unchecked `blockMap.find(bk->name)`
   and dereferences the result without checking for `end()`. The library
   won't stop you from getting this order wrong — it just crashes. Always
   implement `writeBlockRecords()` to call
   `writer->writeBlockRecord(blockName, insUnits)` for every block
   *before* `writeBlocks()` runs (`dxfRW::write()` calls them in the
   right order internally, so as long as both callbacks are implemented,
   this isn't an issue — just don't leave `writeBlockRecords()` empty
   like a first draft of this test did).
3. **`dxfRW` and `dwgRW` are separate classes with similar but not
   identical block APIs.** `dxfRW` uses `writeBlock()` +
   `writeBlockRecord()`. `dwgRW` uses `defineBlock()` +
   `beginBlockContent()`/`endBlockContent()` instead — there's no shared
   base class for the write path, so code targeting one won't compile
   against the other without adjustment. See `write_test.cpp` for both.

## Running it yourself

```bash
cd validation
g++ -std=c++17 -I../third_party/libdxfrw/src -c write_test.cpp -o write_test.o
g++ -std=c++17 -I../third_party/libdxfrw/src -c read_test.cpp -o read_test.o
# link both against the dxfrw static lib produced by the main CMake build:
g++ -std=c++17 write_test.o ../build/libdxfrw.a -o write_test
g++ -std=c++17 read_test.o  ../build/libdxfrw.a -o read_test
./write_test && ./read_test
```
