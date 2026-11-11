# Prepared by ChatGPT: XML regression fixture provenance

`xmlfixtures.h` embeds verbatim bytes from the MIT-licensed
[pyrekordbox repository](https://github.com/dylanljones/pyrekordbox) at commit
`02a208837434e4c05c0a222e8fe6583e6ac0547f`. Keeping the bytes compiled into the
test avoids dependence on the build's source-path remapping or working directory.
The original repository license is retained in `LICENSE.pyrekordbox`.

| Symbol | Original path | Git blob | SHA-256 |
| --- | --- | --- | --- |
| `kRekordbox586Xml` | `.testdata/rekordbox 5/database.xml` | `9cc02bba55772ac3a313c20e586c9eedce5f2179` | `d3f7c9b649ce40b7606de8bd49346f91450c5acafeadeec99b832da6b46ba2f1` |
| `kRekordbox662Xml` | `.testdata/rekordbox 6/database.xml` | `96b71997e45e1b1dfb2e51a61a104d5a69823f1f` | `fed8bf4f63759b03c9627170289e79d140008fe681c783651f36547553368101` |

The PRODUCT labels state 5.8.6 and 6.6.2. Both XML files have been unchanged since
the provider's initial commit `509012710fd6a0cbf144892d59f0cc6ffeef6274` in April
2022. The export process is not independently attested; these are external
regression fixtures, not completed human-export validation or version support
claims. Cue RGB extensions and hot loops are covered by explicitly synthetic
tests; these external XML files do not establish their real-export behavior.

Prepared by ChatGPT for human review and submission.
