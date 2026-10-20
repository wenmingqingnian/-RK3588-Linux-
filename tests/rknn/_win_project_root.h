// Pre-injected macro definition for Windows host testing.
// This file is force-included via `g++ -include` to define
// RKNN_TEST_PROJECT_ROOT as a string literal, working around PowerShell's
// quoting limitations when passing -DMACRO="value" on the command line.
// On Linux boards the path has no non-ASCII chars, so the build script can
// pass -DRKNN_TEST_PROJECT_ROOT=/home/.../RK3588S_CarTerminal directly.
#ifndef RKNN_TEST_PROJECT_ROOT
#define RKNN_TEST_PROJECT_ROOT "C:/rknn_test_link"
#endif
