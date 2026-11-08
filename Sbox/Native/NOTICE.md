# Native dependencies

`thirdparty/minhook` is MinHook v1.3.4 from https://github.com/TsudaKageyu/minhook/tree/v1.3.4 . Its BSD 2-clause license is included in `LICENSE.txt`.

The one local change in `src/hook.c` retains a removed hook's trampoline allocation. The s&box log stubs may still be in flight when a hook is removed; their slots, stubs, trampolines and replacement DLLs stay resident until process exit. Retired slots are not reused (256 creations per process). Active hooks still use MinHook's thread suspension and instruction-pointer relocation during removal. This differs from the reference Godot module's embedded MinHook subset.

`aidebug_native.cpp` adapts the Windows x64 entry/exit emitters in this repository's Godot implementation at commit `84fd57b`. It preserves the four floating argument registers and floating return register, while only sampling the four integer argument registers and integer return register. It does not interpret arbitrary signatures. Integer constants require an integer-compatible return ABI; replacement DLL exports must match the original ABI. Exception unwinding through generated log stubs is not supported.
