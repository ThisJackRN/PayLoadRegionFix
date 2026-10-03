#!/usr/bin/env bash
set -euo pipefail

cd "$(cygpath -u "$REGIONFIX_BUILD_ROOT")"
make
make -C upstream/libiosuhax
make -C upstream/PayloadLoaderInstaller USE_PREBUILT_PAYLOAD=1
make -C upstream/PayloadLoaderInstaller USE_PREBUILT_PAYLOAD=1 DIAGNOSTIC_ONLY=1
