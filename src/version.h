/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Single source of truth for the version. The resource compiler includes this
 * file too, so keep it to plain #defines.
 *
 * On a release, also update the version attribute in src\dofram.manifest by
 * hand (it cannot use these macros) and add an entry to CHANGELOG.md.
 */
#ifndef DOFRAM_VERSION_H
#define DOFRAM_VERSION_H

#define DOFRAM_VERSION_MAJOR 1
#define DOFRAM_VERSION_MINOR 0
#define DOFRAM_VERSION_PATCH 0

#define DOFRAM_VERSION_STR   "1.0.0"
#define DOFRAM_VERSION_WSTR  L"1.0.0"
#define DOFRAM_NAME_WSTR     L"Dofram"
#define DOFRAM_URL_STR       "https://github.com/dogukansahil/Dofram"
#define DOFRAM_URL_WSTR      L"https://github.com/dogukansahil/Dofram"
#define DOFRAM_PUBLISHER     "Dogukan Sahil and Dofram contributors"
#define DOFRAM_PUBLISHER_WSTR L"Dogukan Sahil and Dofram contributors"
#define DOFRAM_COPYRIGHT     "Copyright (C) 2026 Dogukan Sahil and Dofram contributors"
#define DOFRAM_COPYRIGHT_WSTR L"Copyright \x00A9 2026 Dogukan Sahil and Dofram contributors"

/* GitHub API path of the latest release (used by "Check for Updates"). */
#define DOFRAM_API_HOST_WSTR    L"api.github.com"
#define DOFRAM_API_LATEST_WSTR  L"/repos/dogukansahil/Dofram/releases/latest"

#endif
