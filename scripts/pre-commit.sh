#!/usr/bin/env bash

set -euo pipefail

readonly clang_format_version="18.1.8"
readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly project_root="$(cd -- "${script_directory}/.." && pwd)"
readonly preset="${SC_PRECOMMIT_PRESET:-hardware-free-tests}"

if [[ -n "${PYTHON:-}" ]]; then
    python_executable="${PYTHON}"
elif command -v python3 >/dev/null 2>&1; then
    python_executable="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
    python_executable="$(command -v python)"
else
    echo "Python is required to install clang-format and run the test suite." >&2
    exit 1
fi

python_scripts="$("${python_executable}" -c 'import sysconfig; print(sysconfig.get_path("scripts"))')"
clang_format="${SC_CLANG_FORMAT_EXECUTABLE:-${python_scripts}/clang-format}"

if [[ ! -x "${clang_format}" ]] || ! "${clang_format}" --version | grep -Fq "clang-format version ${clang_format_version}"; then
    if ! "${python_executable}" -m pip --version >/dev/null 2>&1; then
        echo "The selected Python environment does not provide pip." >&2
        echo "Activate the project's Python environment or set PYTHON to its interpreter." >&2
        exit 1
    fi
    echo "Installing clang-format ${clang_format_version} into the active Python environment..."
    "${python_executable}" -m pip install "clang-format==${clang_format_version}"
    clang_format="${python_scripts}/clang-format"
fi

cd "${project_root}"

echo "Formatting C++ sources..."
cmake -DSC_CLANG_FORMAT_EXECUTABLE="${clang_format}" -DSC_FORMAT_MODE=format -P cmake/RunClangFormat.cmake

echo "Checking C++ formatting and whitespace..."
cmake -DSC_CLANG_FORMAT_EXECUTABLE="${clang_format}" -DSC_FORMAT_MODE=check -P cmake/RunClangFormat.cmake
git diff --check HEAD

echo "Configuring and building preset '${preset}'..."
cmake --preset "${preset}" -DPython3_EXECUTABLE="${python_executable}"
cmake --build --preset "${preset}" --parallel

echo "Running tests..."
ctest --preset "${preset}"

echo "Pre-commit checks passed. Review git diff before committing."
