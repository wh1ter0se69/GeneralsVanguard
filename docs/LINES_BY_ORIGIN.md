# Lines of code by origin

Measured on `v0.9.74` with [`scripts/tooling/line-origin.py`](../scripts/tooling/line-origin.py). Each line counts
toward the earliest of these that already has it (whitespace-only edits keep the older owner, as `git blame -w`
does): EA's release, TheSuperHackers' mainline (merge base `f51b2804`, or `3fcbbb4d` for fixes ported later), the
Fighter19 → fbraz3 → ammaarreshi line (`c5c8c4d3`), and otherwise this fork. Engine C/C++ is every `.c .cc .cpp .h
.hpp .inl .m .mm` file under `Core/`, `Generals/`, `GeneralsMD/` and `Dependencies/`.

| Engine C/C++ | Lines | Share |
|---|---:|---:|
| EA's 2003 release | 1,408,086 | 71.0% |
| TheSuperHackers | 216,232 | 10.9% |
| Fighter19 / fbraz3 / ammaarreshi, not in upstream | 26,981 | 1.4% |
| This fork | 333,194 | 16.8% |
| **Total** | **1,984,493** | |

Of this fork's 333,194 engine lines, 45,529 are vendored third-party code (Vulkan Memory Allocator, volk, AMD
FidelityFX FSR 1, dr_wav/dr_mp3), 30,576 are SPIR-V shader bytecode generated into headers, 34,867 are tests, and
222,222 are hand-written engine code. Outside the engine C/C++, most lines are this fork's: the build and packaging
scripts, the data generators, and the generated [`THIRD_PARTY_NOTICES.txt`](../THIRD_PARTY_NOTICES.txt).

To reproduce, fetch the base commits (all public) and run the script:

```sh
git fetch https://github.com/TheSuperHackers/GeneralsGameCode 3fcbbb4dcaabb59db23cbdecd2cd7ee93d599248
git fetch https://github.com/ammaarreshi/Generals-Mac-iOS-iPad c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f
python3 scripts/tooling/line-origin.py v0.9.74 \
    EA=0a05454d8574207440a5fb15241b98ad0b435590 \
    TheSuperHackers=f51b28047a8f6f1e3e6f227c32aa96e52d360fef,3fcbbb4dcaabb59db23cbdecd2cd7ee93d599248 \
    lineage=c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f
```

(`0a05454d` is EA's own last commit of the source release, and `f51b2804` is an ancestor of `3fcbbb4d`, so the first
fetch brings both.)

What the method does not see: deleted lines (this counts what is in the tree, not what was removed), code that one
project moved and another then rewrote, and work that the Fighter19 → fbraz3 → ammaarreshi line contributed upstream
before it reached this fork (that counts as TheSuperHackers', which is listed first). A line this fork edited counts
as this fork's.
