# Binding host model

Run from the repository root on Linux/WSL or macOS with Python 3 and a C++17 compiler:

```sh
python3 package/Tb710fuSourceStorage/Test/run-binding-model.py --cxx g++ --sanitizers
```

`--output <directory>` retains the generated input, commands, logs and hash report.
The runner reads the current `package/SourceRootBinding.cpp`, removes only its
`#include` lines and compiles every other line unchanged. The fixture contains no
copy of the production binding logic. Assertions remain enabled.

The narrow model implements retained registry properties/parents, matching
dictionaries and iterators. Tests cover all three binding interfaces, repeated
calls and reference counts, ambiguous UUID/BSD identities, input/range bounds,
cleared failure outputs, allocation/set failures, authentication refusal,
inactive services, iterator invalidity, cycles and the 63/64-parent boundary.
Deterministic hooks mutate properties, identity matches and activity during
authentication; they model interleavings rather than executing kernel threads.

The model substitutes `native_ufs_block_authenticates` and does not verify crypto,
hardware read-only enforcement, real IOKit scheduling or atomic detach/rematch.
The actual target still requires the pinned XNU, IOStorageFamily and VerifiedRoot
interfaces, macOS arm64e compilation, storage dispatch/PAC qualification, kernel
integration and hardware tests. Mount callers must reauthorize the mounted device.
Passing this host model establishes none of those later stages.
