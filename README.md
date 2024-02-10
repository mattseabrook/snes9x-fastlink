*Snes9x - Portable Super Nintendo Entertainment System (TM) emulator*

This is a fork of the official source code repository for the Snes9x project, cleaned up for 2024 and building as `C++ 20` in `VS 2022`. Importantly, a new feature has been implemented that adds a socket server on a dedicated thread that surfaces the entire state RAM of the emulated SNES. This behaves like a standard WWW Rest API on the `localhost:9000/`

**Table-of-Contents**

- [CHANGELOG](#changelog)
  - [v0.1 - 2024-02-09](#v01---2024-02-09)
    - [Lifted the project from VS 2017 to VS 2022](#lifted-the-project-from-vs-2017-to-vs-2022)
    - [Removal of the DirectX SDK](#removal-of-the-directx-sdk)
    - [fmtlib - External Dependency Warnings](#fmtlib---external-dependency-warnings)
    - [Const Correctness](#const-correctness)

# CHANGELOG

## v0.1 - 2024-02-09

### Lifted the project from VS 2017 to VS 2022

- Windows SDK Version changed from `8.1` to `10.0 Latest`
- Platform Toolset changed from `VS 2017 v141_xp` to `VS 2022 v143`
- C++ Language Standard changed from `C++ 17` to `C++ 20`

### Removal of the DirectX SDK

- lOREM iPSUM
### fmtlib - External Dependency Warnings

```
warning C4996: 'stdext::checked_array_iterator<T *>::value_type': warning STL4043: stdext::checked_array_iterator, stdext::unchecked_array_iterator, and related factory functions are non-Standard extensions and will be removed in the future. std::span (since C++20) and gsl::span can be used instead. You can define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING or _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS to suppress this warning.
```

- These warnings were solved by overwriting the `src` and `include` folders in this repository with the `src` and `include` folders from the latest version of `fmt`
  
### Const Correctness

Several instances across the project of errors `C2440` and `C2664`, including variable declarations and function arguments, were updated to conform to the C++ 20 requirement `/Zc:strictStrings`. Compilation errors arose when non-const pointers were initialized with string literals. This was primarily observed in `wsnes9x.cpp` and other files where `TCHAR*` variables were assigned string literals directly. This practice is deemed unsafe in C++ 20 as it allows modification of string literals, which are stored in read-only memory sections of an application.