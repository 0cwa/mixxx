# Rekordbox XML importer seam

`rekordboxxmlparser.h/.cpp` is a read-only parser for the documented Rekordbox
XML V1 interchange shape. It produces independent DTOs for collection tracks,
playlist/folder trees, tempo/beatgrid entries, position marks (cues and loops),
and string artwork references. Collection tracks are retained even when no
playlist references them.

The parser deliberately does not construct `Track` or touch the database. The
current read-only `RekordboxFeature` integration uses these DTOs to populate its
temporary external-library tables after a user selects an XML file; it never
writes the source XML or Mixxx's canonical Collection. Playlist IDs are resolved
after all collection tracks are known, and duplicate track IDs/locations are
reported and skipped. Cue/loop DTOs and artwork references remain available for
the later canonical import/export stages, where path portability, cue-color
semantics, and artwork storage will be decided explicitly.

Malformed XML returns any safely parsed prefix plus diagnostics with line and
column information; no mutation is attempted.
