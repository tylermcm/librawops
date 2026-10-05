# Range-mask canonical metadata correction v1

2026-10-04. Saved graph replay found one error in the independent pre-native
operation byte examples: operation opacity was written as integer JSON `1`.
The existing metadata parser stores opacity as binary64, and the established
canonical serializer writes it as `1.0`. Engine serialization and operation
semantics are unchanged. The three range examples and their independent SHA256
strings now use `"opacity":1.0`; strict normal blend/opacity-one admission remains.

The old oracle/header are preserved byte for byte in
`build-msvc-release/research/before-range-integration-v1/`. A mechanical source
comparison permits exactly the oracle's opacity literal correction. A mechanical
header comparison removes only the six operation JSON/SHA256 lines and proves
every numeric scalar case, settings word, native/mip frame and guide/input bit is
unchanged. Three parsed trees compare equal, and exact JSON strings differ only
by the three opacity literals. No range numeric version or frozen contract changes.

`build-msvc-release/research/range-mask-canonical-correction-v1.json` binds this
correction, current generator/header and preceding archived hashes. Older contract
and native-foundation receipts retain their original hashes and historical
canonical-example limitation. Future acceptance uses the corrected examples;
actual C++ and Python history/serialization must reproduce their exact bytes.
This correction does not complete the original range implementation checkbox.
